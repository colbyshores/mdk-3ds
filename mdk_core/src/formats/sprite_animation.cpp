// ============================================================================
// sprite_animation.cpp — see sprite_animation.h.
// ============================================================================

#include "formats/sprite_animation.h"

#include "formats/bin_reader.h"

namespace mdk {
namespace formats {

namespace {

// The last command that carries literal indices.
const uint8_t kLiteralLast = 0x7F;

// The first command that ends a row.
const uint8_t kEndOfRow = 0xFE;

// The bias of the repeat command's count.
const int32_t kRepeatBias = 0x7C;

} // namespace

bool SpriteAnimation::parse(SpriteAnimation* out, const std::string& name, const uint8_t* bytes,
                            int32_t byteCount, int32_t offset) {
    if (!out || !bytes || offset < 0 || offset + 4 > byteCount) {
        return false;
    }

    SpriteAnimation animation;
    animation.name_ = name;
    animation.bytes_ = bytes;
    animation.byteCount_ = byteCount;
    animation.base_ = offset;
    animation.frameCount_ = (int32_t)binU32(bytes, offset);
    if (animation.frameCount_ < 0) {
        return false;
    }

    *out = animation;
    return true;
}

bool SpriteAnimation::frame(int32_t index, Frame* out) const {
    if (!out || index < 0 || !bytes_) {
        return false;
    }

    // The frame offsets are in the table that follows the frame count.
    const int32_t at = base_ + 4 + index * 4;
    if (at < 0 || at + 4 > byteCount_) {
        return false;
    }

    if (frames_.empty()) {
        frames_.resize((size_t)frameCount_);
        decoded_.resize((size_t)frameCount_, false);
    }
    if (index < frameCount_) {
        if (decoded_[(size_t)index]) {
            *out = frames_[(size_t)index];
            return true;
        }
    }

    Frame frame;
    if (!decode(base_ + (int32_t)binU32(bytes_, at), &frame)) {
        return false;
    }
    if (index < frameCount_) {
        frames_[(size_t)index] = frame;
        decoded_[(size_t)index] = true;
    }
    *out = frame;
    return true;
}

bool SpriteAnimation::decode(int32_t offset, Frame* out) const {
    if (!out || !bytes_ || offset < 0 || offset + 8 > byteCount_) {
        return false;
    }

    const int32_t width = (int32_t)binU16(bytes_, offset);
    const int32_t height = (int32_t)binU16(bytes_, offset + 2);
    if (width < 0 || height < 0) {
        return false;
    }
    const int64_t size = (int64_t)width * (int64_t)height;
    if (size > (int64_t)byteCount_) {
        return false;
    }

    std::vector<uint8_t> indices((size_t)size, 0);
    int32_t p = offset + 8;
    for (int32_t y = 0; y < height; y++) {
        int32_t i = y * width;
        while (true) {
            if (p < 0 || p >= byteCount_) {
                return false;
            }
            const uint8_t command = bytes_[p++];
            // 0xFE and 0xFF BOTH end the row. The C# does not tell them
            // apart, and the game shows what the C# shows.
            if (command >= kEndOfRow) {
                break;
            }
            if (command <= kLiteralLast) {
                const int32_t count = (int32_t)command + 1;
                if (i + count > (int32_t)size || p + count > byteCount_) {
                    return false;
                }
                for (int32_t k = 0; k < count; k++) {
                    indices[(size_t)(i + k)] = bytes_[(size_t)(p + k)];
                }
                i += count;
                p += count;
                continue;
            }
            if (p >= byteCount_) {
                return false;
            }
            const uint8_t value = bytes_[p++];
            const int32_t count = (int32_t)command - kRepeatBias;
            if (count <= 0 || i + count > (int32_t)size) {
                return false;
            }
            for (int32_t k = 0; k < count; k++) {
                indices[(size_t)(i + k)] = value;
            }
            i += count;
        }
    }

    Texture image;
    image.setName(name_);
    image.setWidth(width);
    image.setHeight(height);
    image.setIndices(indices.data(), (int32_t)indices.size());

    out->image = image;
    out->hotspotX = (int32_t)binS16(bytes_, offset + 4);
    out->hotspotY = (int32_t)binS16(bytes_, offset + 6);
    return true;
}

} // namespace formats
} // namespace mdk
