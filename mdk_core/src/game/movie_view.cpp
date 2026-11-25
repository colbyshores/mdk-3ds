// movie_view.cpp -- see movie_view.h. C# src/Mdk.Game/Menu/Ui.cs (PalettedImage),
// VideoPlayer.Draw and IntroSplash.Draw.

#include "game/movie_view.h"

#include <string.h>

#include "formats/palette.h"
#include "game/intro_splash.h"
#include "game/video_player.h"
#include "render/render_types.h"

namespace mdk {
namespace {

// C# `new PalettedImage(renderer, width, height, indices, paletteRgb)`: the
// texture and its palette (Palette.FromRgb's RGBA). The palette is then set
// again through the streamed path, which on the 3DS turns the texture into a
// streamed one before its first draw, and on Linux finds it unchanged.
bool makeImage(MdkRenderer renderer, int32_t width, int32_t height, const uint8_t* indices,
               const uint8_t* rgb, PalettedImage* out) {
    const formats::Palette palette = formats::Palette::fromRgb(rgb, 768);
    const int32_t paletteId = mdk_render_create_palette(renderer, palette.rgba());
    if (paletteId < 0) {
        return false;
    }
    const int32_t textureId =
        mdk_render_create_streamed_texture(renderer, width, height, indices, palette.rgba());
    if (textureId < 0) {
        mdk_render_release_palette(renderer, paletteId);
        return false;
    }
    mdk_render_update_paletted(renderer, textureId, paletteId, nullptr, palette.rgba());
    out->texture = textureId;
    out->palette = paletteId;
    out->width = width;
    out->height = height;
    return true;
}

void releaseImage(MdkRenderer renderer, PalettedImage* image) {
    if (image->ready()) {
        mdk_render_release_texture(renderer, image->texture);
        mdk_render_release_palette(renderer, image->palette);
    }
    *image = PalettedImage();
}

} // namespace

void MovieView::sync(MdkRenderer renderer, const menu::VideoPlayer& player, bool halfRate) {
    if (!player.hasStill()) {
        return;
    }
    // C# ShowStill: a new image when the size changed (an FLC, then an MVE).
    if (image.ready() && (image.width != player.width() || image.height != player.height())) {
        releaseImage(renderer, &image);
    }
    if (!image.ready()) {
        if (makeImage(renderer, player.width(), player.height(), player.indices().data(),
                      player.shownPaletteRgb(), &image)) {
            imageVersion = player.imageVersion();
            paletteVersion = player.paletteVersion();
            uploads++;
        }
        return;
    }
    const bool newPicture = player.imageVersion() != imageVersion;
    const bool newPalette = player.paletteVersion() != paletteVersion;
    if (!newPicture && !newPalette) {
        return;
    }
    // The half rate: every second picture, and always the last one.
    if (halfRate && newPicture && player.playing() && player.imageVersion() - imageVersion < 2u) {
        return;
    }
    const formats::Palette palette = formats::Palette::fromRgb(player.shownPaletteRgb(), 768);
    mdk_render_update_paletted(renderer, image.texture, image.palette,
                               newPicture ? player.indices().data() : nullptr,
                               newPalette ? palette.rgba() : nullptr);
    imageVersion = player.imageVersion();
    paletteVersion = player.paletteVersion();
    uploads++;
}

void MovieView::draw(MdkRenderer renderer, const menu::VideoPlayer& player,
                     float canvasWidth) const {
    if (!image.ready()) {
        return;
    }
    ScreenView view;
    view.layout(canvasWidth, kCanvasHeight, player.viewWidth(), player.viewHeight(), false);
    view.image(renderer, image.texture, image.palette, image.width, image.height,
               player.imageX(), player.imageY());
}

void MovieView::release(MdkRenderer renderer) {
    releaseImage(renderer, &image);
    imageVersion = 0;
    paletteVersion = 0;
}

bool SplashView::open(MdkRenderer renderer, const menu::IntroSplash& splash, bool fade) {
    release(renderer);
    crossFade = fade;
    if (splash.done() || splash.picture().empty()) {
        return false;
    }
    const int32_t w = menu::IntroSplash::kWidth;
    const int32_t h = menu::IntroSplash::kHeight;
    if (!crossFade) {
        // C#: the image is made through palette 0 (black); Draw sets the blend.
        return makeImage(renderer, w, h, splash.picture().data(), splash.paletteRgb(0), &image);
    }
    return makeImage(renderer, w, h, splash.picture().data(), splash.paletteRgb(1), &image) &&
           makeImage(renderer, w, h, splash.picture().data(), splash.paletteRgb(2), &second);
}

void SplashView::draw(MdkRenderer renderer, menu::IntroSplash& splash, float canvasWidth) {
    if (splash.done() || !image.ready()) {
        return;
    }
    ScreenView view;
    view.layoutInside(canvasWidth, kCanvasHeight);
    if (!crossFade) {
        // C# Draw: SetPalette(Blend(...)), then the image at (0, 0).
        const uint8_t* rgb = splash.palette();
        if (!hasLast || memcmp(rgb, lastRgb, sizeof(lastRgb)) != 0) {
            const formats::Palette palette = formats::Palette::fromRgb(rgb, 768);
            mdk_render_update_paletted(renderer, image.texture, image.palette, nullptr,
                                       palette.rgba());
            memcpy(lastRgb, rgb, sizeof(lastRgb));
            hasLast = true;
        }
        view.image(renderer, image.texture, image.palette, image.width, image.height, 0.0f, 0.0f);
        return;
    }
    // The cross-fade: from * (1 - w) + to * w, palette 0 being black.
    int32_t from = 0;
    int32_t to = 0;
    int32_t weight = 0;
    if (!splash.fade(&from, &to, &weight) || !second.ready()) {
        return;
    }
    const float w = (float)weight / 256.0f;
    const PalettedImage* pictures[3] = {nullptr, &image, &second};
    const Colour white{1.0f, 1.0f, 1.0f, 1.0f};
    if (from == to) {
        if (from != 0) {
            const PalettedImage* p = pictures[from];
            view.image(renderer, p->texture, p->palette, p->width, p->height, 0.0f, 0.0f, white);
        }
        return;
    }
    if (from == 0) {
        const PalettedImage* p = pictures[to];
        const Colour tint{w, w, w, 1.0f};
        view.image(renderer, p->texture, p->palette, p->width, p->height, 0.0f, 0.0f, tint);
        return;
    }
    const PalettedImage* a = pictures[from];
    if (to == 0) {
        const Colour tint{1.0f - w, 1.0f - w, 1.0f - w, 1.0f};
        view.image(renderer, a->texture, a->palette, a->width, a->height, 0.0f, 0.0f, tint);
        return;
    }
    const PalettedImage* b = pictures[to];
    view.image(renderer, a->texture, a->palette, a->width, a->height, 0.0f, 0.0f, white);
    const Colour over{1.0f, 1.0f, 1.0f, w};
    view.image(renderer, b->texture, b->palette, b->width, b->height, 0.0f, 0.0f, over);
}

void SplashView::release(MdkRenderer renderer) {
    releaseImage(renderer, &image);
    releaseImage(renderer, &second);
    hasLast = false;
}

} // namespace mdk
