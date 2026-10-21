// ============================================================================
// sniper_screen.cpp — see sniper_screen.h for the format.
// ============================================================================

#include "formats/sniper_screen.h"

#include <string.h>

#include "formats/bin_reader.h"

namespace mdk {
namespace formats {

namespace {

const char kFrameName[] = "SNIPERS1";
const char kMaskName[] = "SNIPERS2";
// The mask's entry has a size field; the frame's does not.
const int32_t kSizeField = 4;
const int32_t kWordSize = 2;
const uint16_t kEnd = 0xFF00;
const uint16_t kShortLiteral = 0xFF00;
const uint16_t kSkip = 0x8000;
const int32_t kSkipMask = 0xFFF;
const int32_t kShortCountMask = 0xFF;
const int32_t kLiteralUnit = 4;

} // namespace

const ArchiveIndex::Entry* SniperScreen::find(const Bni& sprites, const char* name) {
    return sprites.index().find(name);
}

bool SniperScreen::frame(const Bni& sprites, Texture* outTexture) {
    if (!outTexture) {
        return false;
    }

    const ArchiveIndex::Entry* entry = find(sprites, kFrameName);
    if (!entry) {
        return false;
    }

    const std::vector<uint8_t>& bytes = sprites.bytes();
    const int32_t size = kScreenWidth * kScreenHeight;
    if (entry->offset < 0 || (int64_t)entry->offset + size > (int64_t)bytes.size()) {
        return false;
    }

    Texture texture;
    texture.setName(kFrameName);
    texture.setWidth(kScreenWidth);
    texture.setHeight(kScreenHeight);
    texture.setIndices(bytes.data() + entry->offset, size);

    *outTexture = texture;
    return true;
}

bool SniperScreen::mask(const Bni& sprites, Texture* outTexture) {
    if (!outTexture) {
        return false;
    }

    const ArchiveIndex::Entry* entry = find(sprites, kMaskName);
    if (!entry) {
        return false;
    }

    const std::vector<uint8_t>& bytes = sprites.bytes();
    std::vector<uint8_t> indices;
    if (!decodeMask(bytes.data(), (int32_t)bytes.size(), entry->offset + kSizeField, &indices)) {
        return false;
    }

    Texture texture;
    texture.setName(kMaskName);
    texture.setWidth(kViewWidth);
    texture.setHeight(kViewHeight);
    texture.setIndices(indices.data(), (int32_t)indices.size());

    *outTexture = texture;
    return true;
}

bool SniperScreen::decodeMask(const uint8_t* bytes, int32_t byteCount, int32_t offset,
                              std::vector<uint8_t>* outIndices) {
    std::vector<uint8_t> indices((size_t)kViewWidth * (size_t)kViewHeight, 0);
    int32_t pixel = 0;
    while (pixel < (int32_t)indices.size() && offset + kWordSize <= byteCount) {
        const uint16_t word = binU16(bytes, offset);
        offset += kWordSize;
        if (word == kEnd) {
            break;
        }

        int32_t count;
        if ((word & kShortLiteral) == kShortLiteral) {
            count = word & kShortCountMask;
        } else if ((word & kSkip) != 0) {
            // A run of transparent pixels: no bytes are read for it.
            pixel += word & kSkipMask;
            continue;
        } else {
            count = word * kLiteralUnit;
        }

        // The literal indices are read without a check of their own, so a
        // mask that claims more than it holds runs past the end: the C#
        // throws there.
        if (offset + count > byteCount) {
            return false;
        }

        for (int32_t i = 0; i < count && pixel < (int32_t)indices.size(); i++, pixel++) {
            indices[(size_t)pixel] = bytes[offset + i];
        }

        offset += count;
    }

    *outIndices = indices;
    return true;
}

} // namespace formats
} // namespace mdk
