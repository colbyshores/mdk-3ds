#pragma once

// ============================================================================
// screen_view.h -- the original's 600 x 360 screen on the canvas (C#
// src/Mdk.Game/Menu/Ui.cs, ScreenView and PalettedImage's Draw).
//
//   canvas +--------------------------+
//          |   +------------------+   |  origin + view point x scale
//          |   |  600 x 360 view  |   |
//          |   +------------------+   |
//          +--------------------------+
//
// The menus, the briefing and the pause menu are laid out on the 600 x 360
// view and drawn through it: the C#'s Fit.Inside scales the view to fit the
// canvas and centres it. The canvas is 360 units high (render_types.h
// kCanvasHeight) and as wide as the frame's aspect: 480 on the Linux 4:3
// window (scale 0.8, 36 units of black above and below), exactly 600 on the
// 3DS's 5:3 top screen (scale 1).
//
// Drawing only: between mdk_render_canvas_begin and _end. Cold code (menus).
// ============================================================================

#include <stdint.h>

#include "render/render.h"
#include "render/render_types.h"

namespace mdk {

class FontView;

namespace formats {
class Bni;
}

// An image of palette indices with its palette on the GPU (C# PalettedImage).
struct PalettedImage {
    int32_t texture = -1;
    int32_t palette = -1;
    int32_t width = 0;
    int32_t height = 0;
    bool ready() const { return texture >= 0; }
};

// A BNI image entry that carries its own palette (MISC/OPTIONS.BNI's MDKOPT,
// MISC/STATS.BNI's pages): 768 RGB bytes, then u16 width, u16 height, pixels.
// With `systemRgb` (768 bytes of SYS_PAL), colours 0-63 (its first 192 bytes) are
// the system's and 64-255 the image's (C# StatsScreen.PageRgb). False, leaving `out`
// empty, when the entry is missing or short.
bool loadPalettedImage(MdkRenderer renderer, const formats::Bni& bni, const char* name,
                       const uint8_t* systemRgb, PalettedImage* out);

struct ScreenView {
    // C# ScreenView.Size.
    static constexpr float kWidth = 600.0f;
    static constexpr float kHeight = 360.0f;

    float scale = 1.0f;
    float originX = 0.0f;
    float originY = 0.0f;
    // The canvas the view was laid out on (FillCanvas covers it).
    float canvasWidth = kWidth;
    float canvasHeight = kHeight;

    // C# Layout(Fit.Inside): scale to fit, centred.
    void layoutInside(float canvasW, float canvasH);
    // C# Layout(fit, size) (package #20c): a `viewW` x `viewH` view (the 600 x
    // 360 screen, or an MVE's 640 x 480) scaled to fit inside the canvas, or with
    // `fitHeight` to the canvas's height (the loading screen), centred.
    void layout(float canvasW, float canvasH, float viewW, float viewH, bool fitHeight);

    float toCanvasX(float x) const { return originX + x * scale; }
    float toCanvasY(float y) const { return originY + y * scale; }

    // C# ScreenView.Text: a text from x along the baseline y (view units),
    // scaled about the baseline, at `alpha`.
    void text(MdkRenderer renderer, const FontView& font, const uint8_t* text, int32_t length,
              float x, float y, float textScale, float alpha) const;
    // C# ScreenView.Fill: a rectangle of the view.
    void fill(MdkRenderer renderer, float x, float y, float w, float h, const Colour& colour) const;
    // C# ScreenView.FillCanvas: the whole canvas.
    void fillCanvas(MdkRenderer renderer, const Colour& colour) const;
    // C# PalettedImage.Draw: an index texture at its size, at (x, y) of the view.
    void image(MdkRenderer renderer, int32_t texture, int32_t palette, int32_t width,
               int32_t height, float x, float y) const;
    // The same in `tint` (package #20c: the 3DS splash's GPU cross-fade).
    void image(MdkRenderer renderer, int32_t texture, int32_t palette, int32_t width,
               int32_t height, float x, float y, const Colour& tint) const;
    // C# Renderer.FillRect of a CANVAS rectangle (FrameRect's pieces).
    void fillCanvasRect(MdkRenderer renderer, float x, float y, float w, float h,
                        const Colour& colour) const;
};

// .NET's MathF.Round: to the nearest, a half to the even neighbour.
float roundEven(float value);

} // namespace mdk
