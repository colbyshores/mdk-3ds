// ============================================================================
// texture.cpp — see texture.h.
// ============================================================================

#include "formats/texture.h"

#include <string.h>

#include "formats/bin_reader.h"

namespace mdk {
namespace formats {

Texture Texture::parse(const std::string& name, const uint8_t* bytes, int32_t byteCount, int32_t offset) {
    Texture texture;
    texture.name_ = name;
    if (offset < 0 || offset + 4 > byteCount) {
        return texture;
    }
    texture.width_ = (int32_t)binU16(bytes, offset);
    texture.height_ = (int32_t)binU16(bytes, offset + 2);
    const int32_t size = texture.width_ * texture.height_;
    const int32_t start = offset + 4;
    if (size < 0 || start < 0 || start + size > byteCount) {
        return texture;
    }
    // A copy: the texture owns its pixels (see texture.h).
    texture.indices_.assign(bytes + start, bytes + start + size);
    return texture;
}

Texture Texture::parseAnimated(const std::string& name, const uint8_t* bytes, int32_t byteCount,
                               int32_t offset, uint32_t kind) {
    Texture texture;
    texture.name_ = name;
    if (offset < 0 || offset + 8 > byteCount) {
        return texture;
    }
    const int32_t count = (int32_t)binU32(bytes, offset);
    texture.width_ = (int32_t)binU16(bytes, offset + 4);
    texture.height_ = (int32_t)binU16(bytes, offset + 6);
    texture.frameCount_ = count;
    const int32_t size = texture.width_ * texture.height_;
    if (count < 0 || size < 0) {
        return texture;
    }
    const int32_t start = offset + 8;
    if (start + size > byteCount) {
        return texture;
    }

    if ((kind & kDeltas) == 0) {
        // The frames are stored back to back. They are COPIED, because the
        // caller may release the file buffer while the texture lives.
        if (start + size * count > byteCount) {
            return texture;
        }
        texture.indices_.assign(bytes + start, bytes + start + size * count);
        return texture;
    }

    // The delta form: the first frame, then the changes that make each next
    // one.
    //
    // After the first frame comes an f32, then a table of offsets: the
    // control block for change k is at table + u32[k*4], and its data block
    // at table + u32[(count+k)*4]. The control is `u16 start, u16 runs`,
    // then per run `u8 copy, u8 skip`, in units of 4 pixels. Change k turns
    // frame k into frame k+1.
    texture.indices_.assign((size_t)size * (size_t)count, 0);
    if (size > 0) {
        memcpy(texture.indices_.data(), bytes + start, (size_t)size);
    }
    const int32_t table = start + size + 4;
    for (int32_t k = 0; k < count - 1; k++) {
        const int32_t controlAt = table + k * 4;
        const int32_t dataAt = table + (count + k) * 4;
        if (controlAt + 4 > byteCount || dataAt + 4 > byteCount) {
            break;
        }
        const int32_t control = table + (int32_t)binU32(bytes, controlAt);
        int32_t data = table + (int32_t)binU32(bytes, dataAt);
        if (control < 0 || control + 4 > byteCount || data < 0 || data > byteCount) {
            break;
        }
        int32_t destination = (int32_t)binU16(bytes, control) * kDeltaUnit;
        const int32_t runs = (int32_t)binU16(bytes, control + 2);
        // Frame k+1 is frame k with change k applied (the C#'s ApplyDeltas).
        // Applying it to frame k in place instead left frames 1.. empty and
        // frame 0 changed (found by the HD export's parity check, 2026-11-26).
        uint8_t* frame = texture.indices_.data() + (size_t)(k + 1) * (size_t)size;
        memcpy(frame, frame - size, (size_t)size);
        for (int32_t run = 0; run < runs; run++) {
            const int32_t runAt = control + 4 + run * 2;
            if (runAt + 2 > byteCount) {
                break;
            }
            const int32_t copy = (int32_t)bytes[runAt] * kDeltaUnit;
            const int32_t skip = (int32_t)bytes[runAt + 1] * kDeltaUnit;
            for (int32_t i = 0; i < copy; i++) {
                if (destination + i < size && data + i < byteCount) {
                    frame[destination + i] = bytes[data + i];
                }
            }
            data += copy;
            destination += copy + skip;
        }
    }
    return texture;
}

const uint8_t* Texture::frame(int32_t index) const {
    const int32_t size = width_ * height_;
    if (index < 0 || index >= frameCount_ || size <= 0) {
        return nullptr;
    }
    needPixels();
    // Still away (no source, or it failed): no frame, rather than a pointer
    // past an empty buffer.
    if (indices_.size() < (size_t)(index + 1) * (size_t)size) {
        return nullptr;
    }
    return indices_.data() + (size_t)index * (size_t)size;
}

void Texture::setIndices(const uint8_t* indices, int32_t count) {
    away_ = false;
    if (count <= 0 || indices == nullptr) {
        indices_.clear();
        return;
    }
    indices_.assign(indices, indices + count);
}

void Texture::dropPixels() {
    if (indices_.empty()) {
        return;   // nothing to drop: an empty texture is not an evicted one
    }
    if (pinned_) {
        return;   // a bullet hole is in them: a fresh read would not have it
    }
    // swap, not clear(): clear keeps the capacity, and the capacity is the point.
    std::vector<uint8_t>().swap(indices_);
    away_ = true;
}

bool Texture::adoptPixels(Texture* fresh) {
    if (!fresh || fresh->indices_.empty() || fresh->width_ != width_ || fresh->height_ != height_ ||
        fresh->frameCount_ != frameCount_) {
        return false;
    }
    if (pinned_ && !away_) {
        return true;   // kept as they are (pinPixels): the read's are the hole-less ones
    }
    indices_.swap(fresh->indices_);
    away_ = false;
    return true;
}

void Texture::replaceIndex(uint8_t from, uint8_t to) {
    needPixels();
    for (size_t i = 0; i < indices_.size(); i++) {
        if (indices_[i] == from) {
            indices_[i] = to;
        }
    }
}

} // namespace formats
} // namespace mdk
