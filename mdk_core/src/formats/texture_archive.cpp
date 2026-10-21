// ============================================================================
// texture_archive.cpp — see texture_archive.h.
// ============================================================================

#include "formats/texture_archive.h"

#include <string.h>

#include "formats/bin_reader.h"

namespace mdk {
namespace formats {

namespace {

// The kind that says "this entry is a palette colour".
const uint32_t kKindColor = 0xFFFFFFFFu;

// The high bits of the kind, set for an animated texture.
const uint32_t kKindAnimatedMask = 0xFFFF0000u;

// Uploaded with alpha: index 0 is see-through. The effect textures.
const uint32_t kKindKeyed = 1u;

// The palette's black, standing in for index 0 where it is opaque.
const uint8_t kOpaqueBlack = 16;

} // namespace

TextureArchive TextureArchive::parse(const uint8_t* bytes, int32_t byteCount, int32_t baseOffset) {
    return parse(bytes, byteCount, baseOffset, kHeader, Names::AsStored, Zero::ByKind);
}

TextureArchive TextureArchive::parse(const uint8_t* bytes, int32_t byteCount, int32_t baseOffset,
                                     int32_t header, Names names, Zero zero) {
    TextureArchive archive;

    BinReader reader(bytes, byteCount, baseOffset + header);
    const uint32_t count = reader.u32();
    for (uint32_t i = 0; i < count; i++) {
        char rawName[9];
        reader.name(rawName, 8);
        std::string name(rawName);
        if (names == Names::Upper) {
            for (size_t c = 0; c < name.size(); c++) {
                if (name[c] >= 'a' && name[c] <= 'z') {
                    name[c] = (char)(name[c] - 'a' + 'A');
                }
            }
        }
        const uint32_t kind = reader.u32();
        const uint32_t value = reader.u32();
        reader.f32();   // an unused float the archive carries
        const int32_t offset = baseOffset + (int32_t)reader.u32();

        if (kind == kKindColor) {
            archive.colors_.push_back(std::make_pair(name, (int32_t)value));
            continue;
        }

        if ((kind & kKindAnimatedMask) != 0) {
            archive.textures_.push_back(std::make_pair(
                name, Texture::parseAnimated(name, bytes, byteCount, offset, kind)));
            continue;
        }

        Texture texture = Texture::parse(name, bytes, byteCount, offset);
        // LEVEL8's walls paint black with index 0 (I2_WALL1): opaque unless
        // the texture is keyed.
        if (zero == Zero::ByKind && (kind & kKindKeyed) == 0) {
            texture.replaceIndex(0, kOpaqueBlack);
        }
        archive.textures_.push_back(std::make_pair(name, texture));
    }

    return archive;
}

const Texture* TextureArchive::findTexture(const std::string& name) const {
    for (const std::pair<std::string, Texture>& entry : textures_) {
        if (entry.first == name) {
            return &entry.second;
        }
    }
    return nullptr;
}

int32_t TextureArchive::findColor(const std::string& name) const {
    for (const std::pair<std::string, int32_t>& entry : colors_) {
        if (entry.first == name) {
            return entry.second;
        }
    }
    return -1;
}

void TextureArchive::add(const std::string& name, const Texture& texture) {
    for (size_t i = 0; i < textures_.size(); i++) {
        if (textures_[i].first == name) {
            textures_[i].second = texture;
            return;
        }
    }

    textures_.push_back(std::make_pair(name, texture));
}

} // namespace formats
} // namespace mdk
