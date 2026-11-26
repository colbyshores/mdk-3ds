// loading_screen.cpp -- see loading_screen.h. C# src/Mdk.Game/Menu/LoadingScreen.cs
// (Show) and Renderer.FrameRect.

#include "game/loading_screen.h"

#include <stdio.h>
#include <string.h>

#include "formats/font.h"
#include "formats/fti.h"
#include "formats/mdk_data.h"
#include "formats/palette.h"
#include "formats/texture.h"
#include "game/font_view.h"

namespace mdk {

LoadingScreen::LoadingScreen() {}
LoadingScreen::~LoadingScreen() {}

bool LoadingScreen::load(MdkRenderer renderer, const formats::MdkData& data, int32_t level) {
    char path[32];
    snprintf(path, sizeof(path), "MISC/LOAD_%d.LBB", (int)level);
    std::vector<uint8_t> bytes;
    // C# `bytes.Length > PaletteSize`.
    if (!data.read(path, &bytes) || (int32_t)bytes.size() <= kPaletteSize) {
        return false;
    }
    const formats::Palette palette = formats::Palette::fromRgb(bytes.data(), kPaletteSize);
    memcpy(rgba, palette.rgba(), sizeof(rgba));
    char name[16];
    snprintf(name, sizeof(name), "LOAD_%d", (int)level);
    const formats::Texture texture =
        formats::Texture::parse(name, bytes.data(), (int32_t)bytes.size(), kPaletteSize);
    if (texture.width() <= 0 || texture.height() <= 0 || !texture.indices()) {
        return false;
    }
    const int32_t paletteId = mdk_render_create_palette(renderer, rgba);
    if (paletteId < 0) {
        return false;
    }
    const int32_t textureId = mdk_render_create_index_texture(
        renderer, texture.width(), texture.height(), texture.indices(), rgba);
    if (textureId < 0) {
        mdk_render_release_palette(renderer, paletteId);
        return false;
    }
    image.texture = textureId;
    image.palette = paletteId;
    image.width = texture.width();
    image.height = texture.height();

    // C# `new Fonts(ui.Renderer, ui.Fti, palette).Big` and LOAD_MSG. Without them
    // the picture and the bar still draw.
    std::vector<uint8_t> ftiBytes;
    formats::Fti fti;
    std::vector<uint8_t> bigBytes;
    font.reset(new formats::Font());
    if (data.read("MISC/MDKFONT.FTI", &ftiBytes) &&
        formats::Fti::parse(&fti, std::move(ftiBytes)) && fti.getBytes("FONTBIG", &bigBytes) &&
        formats::Font::parse(bigBytes.data(), (int32_t)bigBytes.size(), 6, font.get())) {
        fontView.reset(new FontView());
        if (!fontView->load(renderer, *font, paletteId, rgba)) {
            fontView.reset();
        }
        text = fti.getTextBytes("LOAD_MSG");
    }
    return true;
}

void LoadingScreen::release(MdkRenderer renderer) {
    if (fontView && fontView->ready()) {
        mdk_render_release_texture(renderer, fontView->texture());
    }
    fontView.reset();
    if (image.ready()) {
        mdk_render_release_texture(renderer, image.texture);
        mdk_render_release_palette(renderer, image.palette);
    }
    image = PalettedImage();
}

// C# Colour(palette, index): the entry's RGB, opaque.
static Colour colourOf(const uint8_t* rgba, int32_t index) {
    Colour c;
    c.r = (float)rgba[index * 4] / 255.0f;
    c.g = (float)rgba[index * 4 + 1] / 255.0f;
    c.b = (float)rgba[index * 4 + 2] / 255.0f;
    c.a = 1.0f;
    return c;
}

// C# Show's drawing (Present is the frontend's).
void LoadingScreen::draw(MdkRenderer renderer, float canvasWidth, float progress) const {
    if (!image.ready()) {
        return;
    }
    ScreenView view;
    view.layout(canvasWidth, kCanvasHeight, ScreenView::kWidth, ScreenView::kHeight, true);
    view.image(renderer, image.texture, image.palette, image.width, image.height, kImageX,
               kImageY);
    if (fontView && fontView->ready() && !text.empty()) {
        const int32_t width = fontView->width(text.data(), (int32_t)text.size());
        view.text(renderer, *fontView, text.data(), (int32_t)text.size(),
                  (ScreenView::kWidth - (float)width) / 2.0f, kTextY, 1.0f, 1.0f);
    }
    const float clamped = progress < 0.0f ? 0.0f : (progress > 1.0f ? 1.0f : progress);
    view.fill(renderer, kBarX, kBarY, kBarWidth * clamped, kBarHeight, colourOf(rgba, kBarFill));
    // C# Renderer.FrameRect(view.ToCanvas(Bar), view.Scale, colour): four strips
    // of the view's scale, top, bottom, left, right.
    const float x = view.toCanvasX(kBarX);
    const float y = view.toCanvasY(kBarY);
    const float w = kBarWidth * view.scale;
    const float h = kBarHeight * view.scale;
    const float line = view.scale;
    const Colour frame = colourOf(rgba, kBarFrame);
    view.fillCanvasRect(renderer, x, y, w, line, frame);
    view.fillCanvasRect(renderer, x, y + h - line, w, line, frame);
    view.fillCanvasRect(renderer, x, y + line, line, h - 2.0f * line, frame);
    view.fillCanvasRect(renderer, x + w - line, y + line, line, h - 2.0f * line, frame);
}

} // namespace mdk
