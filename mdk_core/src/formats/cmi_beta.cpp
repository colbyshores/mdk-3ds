// ============================================================================
// cmi_beta.cpp — see cmi_beta.h.
// ============================================================================

#include "formats/cmi_beta.h"

#include <string.h>

#include "formats/ascii_case.h"
#include "formats/bin_reader.h"
#include "formats/cmi.h"
#include "formats/script_decoder_beta.h"

namespace mdk {
namespace formats {

const int32_t kBetaPathStepSize = 12;
const int32_t kBetaPathKeySize = 40;
const int32_t kBetaPathCountSize = 4;

namespace {

// Where the demo's file has its directories: the count is at 4, because
// the file starts with a size word and no header.
const int32_t kBetaDirectoryOffset = 4;

// The longest name a directory entry can have: its length is a byte.
const int32_t kMaxNameLength = 255;

void appendU32(std::vector<uint8_t>* out, uint32_t value) {
    const uint8_t bytes[4] = {
        (uint8_t)(value & 0xFFu),
        (uint8_t)((value >> 8) & 0xFFu),
        (uint8_t)((value >> 16) & 0xFFu),
        (uint8_t)((value >> 24) & 0xFFu),
    };
    out->insert(out->end(), bytes, bytes + 4);
}

void appendI32(std::vector<uint8_t>* out, int32_t value) {
    appendU32(out, (uint32_t)value);
}

void appendF32(std::vector<uint8_t>* out, float value) {
    uint32_t bits = 0;
    memcpy(&bits, &value, 4);
    appendU32(out, bits);
}

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

bool Cmi::parseBeta(Cmi* out, const uint8_t* bytes, int32_t byteCount) {
    if (!out) {
        return false;
    }
    std::vector<uint8_t> copy(bytes, bytes + byteCount);
    return parseBeta(out, std::move(copy));
}

bool Cmi::parseBeta(Cmi* out, std::vector<uint8_t>&& bytes) {
    if (!out) {
        return false;
    }
    Cmi cmi;
    cmi.bytes_ = std::move(bytes);
    cmi.dialect_ = ScriptDialect::Beta1996;
    if (!cmi.readDirectories(kBetaDirectoryOffset, true)) {
        return false;
    }
    if (!cmi.readBetaArenas()) {
        return false;
    }
    cmi.convertPaths();
    *out = std::move(cmi);
    return true;
}

bool Cmi::readBetaArenas() {
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
        std::string music;
        if (!readPascalName(&record, &music)) {
            return false;
        }
        setText(&arenaMusic_, arenas[i], toUpperAscii(music));

        std::string ambience;
        if (!readPascalName(&record, &ambience)) {
            return false;
        }
        setText(&arenaAmbience_, arenas[i], toUpperAscii(ambience));

        // The demo's arenas have no scripts yet.
        setOffset(&arenaScripts_, arenas[i], 0);
    }
    return true;
}

void Cmi::convertPaths() {
    // The paths are found in the file as it is now, so this has to run
    // BEFORE the spline records are appended.
    const std::vector<int32_t> paths = scriptBetaPaths(*this);

    std::vector<uint8_t> bytes = bytes_;
    for (size_t i = 0; i < paths.size(); i++) {
        const int32_t path = paths[i];
        // The new offset of the path is where its spline record starts.
        int32_t newOffset = 0;
        bool known = false;
        for (size_t k = 0; k < betaPaths_.size(); k++) {
            if (betaPaths_[k].first == path) {
                newOffset = betaPaths_[k].second;
                known = true;
                break;
            }
        }
        if (!known) {
            newOffset = (int32_t)bytes.size();
            betaPaths_.push_back(std::make_pair(path, newOffset));
        }

        const std::vector<uint8_t> record = betaSplineOf(bytes_.data(), (int32_t)bytes_.size(), path);
        bytes.insert(bytes.end(), record.begin(), record.end());
    }
    bytes_ = std::move(bytes);
}

std::vector<uint8_t> betaSplineOf(const uint8_t* source, int32_t byteCount, int32_t path) {
    std::vector<uint8_t> record;
    if (!source || path < 0 || path + kBetaPathCountSize > byteCount) {
        return record;
    }

    const int32_t count = (int32_t)binU32(source, path);
    std::vector<Vec3f> positions;
    Vec3f position;
    for (int32_t i = 0; i < count; i++) {
        const int32_t at = path + kBetaPathCountSize + i * kBetaPathStepSize;
        if (at < 0 || at + 12 > byteCount) {
            return std::vector<uint8_t>();
        }
        const Vec3f step{binF32(source, at), binF32(source, at + 4), binF32(source, at + 8)};
        // The first step is the position; the rest are steps from it.
        if (i == 0) {
            position = step;
        } else {
            position = Vec3f{position.x + step.x, position.y + step.y, position.z + step.z};
        }
        positions.push_back(position);
    }

    // A path with no steps still has a position, and the spline starts
    // again from its first position after the last frame.
    if (positions.empty()) {
        positions.push_back(Vec3f{0.0f, 0.0f, 0.0f});
    }
    positions.push_back(positions[0]);

    record.reserve((size_t)kBetaPathCountSize + positions.size() * (size_t)kBetaPathKeySize);
    appendU32(&record, (uint32_t)positions.size());
    for (size_t i = 0; i < positions.size(); i++) {
        const size_t before = (i > 0) ? i - 1 : 0;
        const size_t after = (i + 1 < positions.size()) ? i + 1 : positions.size() - 1;
        const Vec3f incoming{positions[i].x - positions[before].x, positions[i].y - positions[before].y,
                            positions[i].z - positions[before].z};
        const Vec3f outgoing{positions[after].x - positions[i].x, positions[after].y - positions[i].y,
                            positions[after].z - positions[i].z};
        appendI32(&record, (int32_t)i);
        appendF32(&record, positions[i].x);
        appendF32(&record, positions[i].y);
        appendF32(&record, positions[i].z);
        appendF32(&record, incoming.x);
        appendF32(&record, incoming.y);
        appendF32(&record, incoming.z);
        appendF32(&record, outgoing.x);
        appendF32(&record, outgoing.y);
        appendF32(&record, outgoing.z);
    }
    return record;
}

} // namespace formats
} // namespace mdk
