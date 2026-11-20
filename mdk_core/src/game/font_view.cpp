// font_view.cpp -- see font_view.h.

#include "game/font_view.h"

#include <string.h>

namespace mdk {

bool FontView::load(MdkRenderer renderer, const formats::Font& font, int32_t palette,
                    const uint8_t* paletteRgba) {
    font_ = font;
    palette_ = palette;

    // The atlas: glyphs side by side, one column apart, tops at
    // (max ascent - ascent) within their row, wrapping to a new row
    // before the row would pass kAtlasRowWidth (the PICA200's 1024 px
    // texture limit; the oracle's single row is ~4400 px for FONTBIG).
    const int32_t kAtlasRowWidth = 1024;
    int32_t maxAscent = 0;
    int32_t maxDescent = 0;
    for (int32_t c = 0; c < 256; c++) {
        const formats::Font::Glyph* glyph = font.glyph((uint8_t)c);
        if (!glyph) {
            continue;
        }
        if (glyph->ascent > maxAscent) {
            maxAscent = glyph->ascent;
        }
        if (glyph->descent > maxDescent) {
            maxDescent = glyph->descent;
        }
    }
    const int32_t rowHeight = maxAscent + maxDescent + 1;
    int32_t rows = 1;
    {
        int32_t at = 0;
        for (int32_t c = 0; c < 256; c++) {
            const formats::Font::Glyph* glyph = font.glyph((uint8_t)c);
            if (!glyph) {
                continue;
            }
            if (at > 0 && at + glyph->width > kAtlasRowWidth) {
                rows++;
                at = 0;
            }
            at += glyph->width + 1;
        }
    }
    const int32_t atlasWidth = kAtlasRowWidth;
    // Power-of-two height: the PICA200's tiled texture layout is
    // defined on power-of-two sizes (the HUD's sprites are too).
    int32_t atlasHeight = 1;
    while (atlasHeight < rows * rowHeight) {
        atlasHeight *= 2;
    }
    std::vector<uint8_t> indices((size_t)atlasWidth * (size_t)atlasHeight, 0);
    int32_t atX = 0;
    int32_t atY = 0;
    for (int32_t c = 0; c < 256; c++) {
        const formats::Font::Glyph* glyph = font.glyph((uint8_t)c);
        if (!glyph) {
            continue;
        }
        if (atX > 0 && atX + glyph->width > kAtlasRowWidth) {
            atX = 0;
            atY += rowHeight;
        }
        const int32_t top = atY + (maxAscent - glyph->ascent);
        const int32_t glyphRows = glyph->ascent + glyph->descent + 1;
        for (int32_t row = 0; row < glyphRows; row++) {
            memcpy(&indices[(size_t)(top + row) * (size_t)atlasWidth + (size_t)atX],
                   &glyph->indices[(size_t)row * (size_t)glyph->width],
                   (size_t)glyph->width);
        }
        x_[c] = atX;
        y_[c] = top;
        atX += glyph->width + 1;
    }

    texture_ = mdk_render_create_index_texture(renderer, atlasWidth, atlasHeight,
                                               indices.data(), paletteRgba);
    if (texture_ < 0) {
        return false;
    }
    maxAscent_ = maxAscent;
    atlasWidth_ = (float)atlasWidth;
    atlasHeight_ = (float)atlasHeight;
    return true;
}

float FontView::draw(MdkRenderer renderer, const char* text, float x, float y, float scale,
                     const Colour& tint) const {
    return draw(renderer, (const uint8_t*)text, (int32_t)strlen(text), x, y, scale, tint);
}

float FontView::draw(MdkRenderer renderer, const uint8_t* text, int32_t length, float x, float y,
                     float scale, const Colour& tint) const {
    float pen = x;
    for (const uint8_t* p = text; p != text + length; ++p) {
        const formats::Font::Glyph* glyph = font_.glyph(*p);
        if (!glyph) {
            pen += (float)font_.spaceWidth() * scale;
            continue;
        }
        const float height = (float)(glyph->ascent + glyph->descent + 1);
        const float u0 = (float)x_[*p] / atlasWidth_;
        const float u1 = (float)(x_[*p] + glyph->width) / atlasWidth_;
        const float v0 = (float)y_[*p] / atlasHeight_;
        const float v1 = v0 + height / atlasHeight_;
        const float top = y - (float)glyph->ascent * scale;
        CanvasQuad q;
        q.x0 = pen; q.y0 = top;
        q.x1 = pen + (float)glyph->width * scale; q.y1 = top;
        q.x2 = pen + (float)glyph->width * scale; q.y2 = top + height * scale;
        q.x3 = pen; q.y3 = top + height * scale;
        q.u0 = u0; q.v0 = v0;
        q.u1 = u1; q.v1 = v0;
        q.u2 = u1; q.v2 = v1;
        q.u3 = u0; q.v3 = v1;
        q.colour = tint;
        q.texture = texture_;
        q.palette = palette_;
        mdk_render_canvas_quad(renderer, &q);
        pen += (float)glyph->width * scale;
    }
    return pen;
}

} // namespace mdk
