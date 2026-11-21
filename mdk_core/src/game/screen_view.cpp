// screen_view.cpp -- see screen_view.h. C# Ui.cs ScreenView (:117-150) and
// PalettedImage.Draw (:205-207).

#include "game/screen_view.h"

#include <math.h>

#include <string.h>

#include <vector>

#include "formats/bni.h"
#include "formats/palette.h"
#include "formats/texture.h"
#include "game/font_view.h"

namespace mdk {
namespace {

void quad(MdkRenderer renderer, float x, float y, float w, float h, const Colour& colour,
          int32_t texture, int32_t palette) {
    CanvasQuad q;
    q.x0 = x; q.y0 = y;
    q.x1 = x + w; q.y1 = y;
    q.x2 = x + w; q.y2 = y + h;
    q.x3 = x; q.y3 = y + h;
    q.u0 = 0.0f; q.v0 = 0.0f;
    q.u1 = 1.0f; q.v1 = 0.0f;
    q.u2 = 1.0f; q.v2 = 1.0f;
    q.u3 = 0.0f; q.v3 = 1.0f;
    q.colour = colour;
    q.texture = texture;
    q.palette = palette;
    mdk_render_canvas_quad(renderer, &q);
}

} // namespace

float roundEven(float value) {
    // nearbyintf honours the rounding mode, which is round-to-nearest-even
    // unless someone changed it; this build never does (AGENTS.md R1 sets FZ and
    // DN only).
    return nearbyintf(value);
}

void ScreenView::layoutInside(float canvasW, float canvasH) {
    canvasWidth = canvasW;
    canvasHeight = canvasH;
    const float sx = canvasW / kWidth;
    const float sy = canvasH / kHeight;
    scale = sx < sy ? sx : sy;
    originX = (canvasW - kWidth * scale) / 2.0f;
    originY = (canvasH - kHeight * scale) / 2.0f;
}

void ScreenView::layout(float canvasW, float canvasH, float viewW, float viewH, bool fitHeight) {
    canvasWidth = canvasW;
    canvasHeight = canvasH;
    const float sx = canvasW / viewW;
    const float sy = canvasH / viewH;
    scale = fitHeight ? sy : (sx < sy ? sx : sy);
    originX = (canvasW - viewW * scale) / 2.0f;
    originY = (canvasH - viewH * scale) / 2.0f;
}

void ScreenView::text(MdkRenderer renderer, const FontView& font, const uint8_t* text,
                      int32_t length, float x, float y, float textScale, float alpha) const {
    if (!font.ready() || length <= 0) {
        return;
    }
    const Colour tint{1.0f, 1.0f, 1.0f, alpha};
    font.draw(renderer, text, length, toCanvasX(x), toCanvasY(y), textScale * scale, tint);
}

void ScreenView::fill(MdkRenderer renderer, float x, float y, float w, float h,
                      const Colour& colour) const {
    quad(renderer, toCanvasX(x), toCanvasY(y), w * scale, h * scale, colour, -1, -1);
}

void ScreenView::fillCanvas(MdkRenderer renderer, const Colour& colour) const {
    quad(renderer, 0.0f, 0.0f, canvasWidth, canvasHeight, colour, -1, -1);
}

void ScreenView::image(MdkRenderer renderer, int32_t texture, int32_t palette, int32_t width,
                       int32_t height, float x, float y) const {
    if (texture < 0) {
        return;
    }
    const Colour white{1.0f, 1.0f, 1.0f, 1.0f};
    quad(renderer, toCanvasX(x), toCanvasY(y), (float)width * scale, (float)height * scale, white,
         texture, palette);
}

void ScreenView::image(MdkRenderer renderer, int32_t texture, int32_t palette, int32_t width,
                       int32_t height, float x, float y, const Colour& tint) const {
    if (texture < 0) {
        return;
    }
    quad(renderer, toCanvasX(x), toCanvasY(y), (float)width * scale, (float)height * scale, tint,
         texture, palette);
}

void ScreenView::fillCanvasRect(MdkRenderer renderer, float x, float y, float w, float h,
                                const Colour& colour) const {
    quad(renderer, x, y, w, h, colour, -1, -1);
}

bool loadPalettedImage(MdkRenderer renderer, const formats::Bni& bni, const char* name,
                       const uint8_t* systemRgb, PalettedImage* out) {
    // C# PaletteSize, StatsScreen.SystemColours (BYTES: colours 0-63).
    const int32_t kPaletteSize = 768;
    const int32_t kSystemColours = 192;
    *out = PalettedImage();
    const formats::ArchiveIndex::Entry* entry = bni.index().find(name);
    const std::vector<uint8_t>& bytes = bni.bytes();
    if (!entry || entry->offset < 0 || entry->size < kPaletteSize + 4 ||
        (int64_t)entry->offset + kPaletteSize + 4 > (int64_t)bytes.size()) {
        return false;
    }
    uint8_t rgb[768];
    memcpy(rgb, &bytes[(size_t)entry->offset], (size_t)kPaletteSize);
    if (systemRgb) {
        memcpy(rgb, systemRgb, (size_t)kSystemColours);
    }
    const formats::Texture texture = formats::Texture::parse(
        name, bytes.data(), (int32_t)bytes.size(), entry->offset + kPaletteSize);
    if (texture.width() <= 0 || texture.height() <= 0 || !texture.indices()) {
        return false;
    }
    const formats::Palette palette = formats::Palette::fromRgb(rgb, kPaletteSize);
    const int32_t paletteId = mdk_render_create_palette(renderer, palette.rgba());
    if (paletteId < 0) {
        return false;
    }
    const int32_t textureId = mdk_render_create_index_texture(
        renderer, texture.width(), texture.height(), texture.indices(), palette.rgba());
    if (textureId < 0) {
        mdk_render_release_palette(renderer, paletteId);
        return false;
    }
    out->texture = textureId;
    out->palette = paletteId;
    out->width = texture.width();
    out->height = texture.height();
    return true;
}

} // namespace mdk
