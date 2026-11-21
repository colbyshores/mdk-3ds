#pragma once

// ============================================================================
// loading_screen.h -- the loading screen (C# src/Mdk.Game/Menu/LoadingScreen.cs;
// level_load 0x41b0c0, drawn by 0x422a10) on the 600 x 360 screen, fitted to
// the canvas's HEIGHT (ScreenView.Fit.Height): MISC/LOAD_n.LBB (a 768-byte
// palette, u16 width, height, pixels; 200 x 200) at (200, 25), LOAD_MSG
// ("Loading") in FONTBIG, in the LBB's palette, centred at y 260, and a
// progress bar from x 10 to 590, y 290 to 310 (colour 3, framed by 4).
//
// The C#'s Play draws ONE frame of it, at progress 0, between the old screen's
// teardown and the level's load (Game.cs:478-483); so do the frontends. The
// LBBs exist for LEVEL3-8; without one the frame is black (the C#'s
// `bytes.Length > PaletteSize` test). Not ported: the 1996 demo's screens (the
// frontends do not load the beta demo).
//
// load() makes the picture, its palette and the font atlas on the renderer,
// inside the caller's scope; draw() is the frame's canvas (between
// mdk_render_canvas_begin and _end). Cold code (once per level).
// ============================================================================

#include <stdint.h>

#include <memory>
#include <vector>

#include "game/screen_view.h"
#include "render/render.h"
#include "render/render_types.h"

namespace mdk {

class FontView;

namespace formats {
class Font;
class MdkData;
}

struct LoadingScreen {
    // C# ImagePosition, TextY, Bar, BarFill, BarFrame.
    static constexpr float kImageX = 200.0f;
    static constexpr float kImageY = 25.0f;
    static constexpr float kTextY = 260.0f;
    static constexpr float kBarX = 10.0f;
    static constexpr float kBarY = 290.0f;
    static constexpr float kBarWidth = 580.0f;
    static constexpr float kBarHeight = 20.0f;
    static constexpr int32_t kBarFill = 3;
    static constexpr int32_t kBarFrame = 4;
    static constexpr int32_t kPaletteSize = 768;

    LoadingScreen();
    ~LoadingScreen();
    LoadingScreen(const LoadingScreen&) = delete;
    LoadingScreen& operator=(const LoadingScreen&) = delete;

    // LOAD_<level>.LBB, MDKFONT.FTI's FONTBIG and LOAD_MSG, made on `renderer`.
    // False (nothing to draw: a black frame) without the LBB.
    bool load(MdkRenderer renderer, const formats::MdkData& data, int32_t level);
    // The frame's canvas: the picture, the text, the bar at `progress` (0-1)
    // and its frame. The view is laid out here (Fit.Height).
    void draw(MdkRenderer renderer, float canvasWidth, float progress) const;

    bool ready() const { return image.ready(); }

    PalettedImage image;
    // The LBB's palette (RGBA, 256 entries) and the text.
    uint8_t rgba[256 * 4] = {};
    std::vector<uint8_t> text;
    std::unique_ptr<formats::Font> font;
    std::unique_ptr<FontView> fontView;
};

} // namespace mdk
