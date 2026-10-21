// ============================================================================
// font.cpp — see font.h for the format and what the C# does.
// ============================================================================

#include "formats/font.h"

#include <string.h>

namespace mdk {
namespace formats {

bool Font::parse(const uint8_t* data, int32_t byteCount, int32_t spaceWidth, Font* outFont) {
    if (!outFont) {
        return false;
    }

    Font font;
    font.spaceWidth_ = spaceWidth;
    font.glyphs_.resize(kCharacterCount);
    font.present_.assign(kCharacterCount, 0);

    for (int32_t c = 0; c < kCharacterCount; c++) {
        const int32_t at = c * 4;
        if (at + 4 > byteCount) {
            return false;
        }

        // The C#'s (int) cast of a u32: an offset above 2^31 becomes
        // negative, and the C# then indexes the array with it and throws.
        // Here it is simply out of the file.
        uint32_t raw = 0;
        memcpy(&raw, data + at, 4);
        const int32_t offset = (int32_t)raw;
        if (offset == 0) {
            continue;
        }

        if (offset < 0 || offset + 3 > byteCount) {
            return false;
        }

        const int32_t ascent = (int32_t)(int8_t)data[offset];
        const int32_t descent = (int32_t)(int8_t)data[offset + 1];
        const int32_t width = (int32_t)data[offset + 2];

        // A negative size means the glyph's rows and columns do not add up:
        // the C#'s AsSpan(offset + 3, size) throws on a negative length.
        const int32_t size = (ascent + descent + 1) * width;
        if (size < 0 || offset + 3 + size > byteCount) {
            return false;
        }

        Glyph glyph;
        glyph.ascent = ascent;
        glyph.descent = descent;
        glyph.width = width;
        glyph.indices.assign(data + offset + 3, data + offset + 3 + size);

        font.glyphs_[c] = glyph;
        font.present_[c] = 1;
    }

    *outFont = font;
    return true;
}

const Font::Glyph* Font::glyph(int32_t character) const {
    if (character < 0 || character >= kCharacterCount || !present_[character]) {
        return nullptr;
    }

    return &glyphs_[character];
}

int32_t Font::width(const uint8_t* text, int32_t length) const {
    int32_t width = 0;
    for (int32_t i = 0; i < length; i++) {
        const Glyph* g = glyph(text[i]);
        width += g ? g->width : spaceWidth_;
    }

    return width;
}

int32_t Font::width(const std::string& text) const {
    return width((const uint8_t*)text.data(), (int32_t)text.size());
}

} // namespace formats
} // namespace mdk
