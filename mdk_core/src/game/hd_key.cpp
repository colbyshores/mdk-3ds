// ============================================================================
// hd_key.cpp -- see hd_key.h.
// ============================================================================

#include "game/hd_key.h"

#include "formats/palette.h"
#include "formats/texture.h"
#include "util/sha256.h"

namespace mdk {

namespace {

void putInt32LittleEndian(int32_t value, uint8_t out[4]) {
    const uint32_t bits = (uint32_t)value;
    out[0] = (uint8_t)bits;
    out[1] = (uint8_t)(bits >> 8);
    out[2] = (uint8_t)(bits >> 16);
    out[3] = (uint8_t)(bits >> 24);
}

} // namespace

std::string hdKeyOf(const formats::Texture& texture, const formats::Palette& palette) {
    bool used[formats::Palette::kSize] = {};
    const uint8_t* indices = texture.indices();
    const int32_t count = texture.indexCount();
    for (int32_t i = 0; i < count; i++) {
        used[indices[i]] = true;
    }
    // Index 0 is always clear: its colour shows nowhere.
    used[0] = false;

    std::vector<uint8_t> colours;
    const uint8_t* rgba = palette.rgba();
    for (int32_t i = 0; i < formats::Palette::kSize; i++) {
        if (used[i]) {
            colours.push_back((uint8_t)i);
            colours.push_back(rgba[i * 4 + 0]);
            colours.push_back(rgba[i * 4 + 1]);
            colours.push_back(rgba[i * 4 + 2]);
        }
    }

    uint8_t header[12];
    putInt32LittleEndian(texture.width(), header);
    putInt32LittleEndian(texture.height(), header + 4);
    putInt32LittleEndian(texture.frameCount(), header + 8);

    Sha256 hash;
    hash.update(header, sizeof(header));
    if (count > 0) {
        hash.update(indices, (size_t)count);
    }
    if (!colours.empty()) {
        hash.update(colours.data(), colours.size());
    }
    uint8_t digest[32];
    hash.finish(digest);

    static const char* const kHex = "0123456789abcdef";
    std::string key;
    for (int i = 0; i < 8; i++) {
        key += kHex[digest[i] >> 4];
        key += kHex[digest[i] & 0xF];
    }
    return key;
}

} // namespace mdk
