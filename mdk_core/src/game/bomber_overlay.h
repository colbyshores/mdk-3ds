#pragma once

// ============================================================================
// bomber_overlay.h — the XE bomber's HUD (C# src/Mdk.Game/Hud/BomberOverlay.cs;
// 0x46be98; bomber_overlay.gd), package #11.
//
// While the bomber's controls are unlocked (scripts/bomber.h, Bomber::shown) the
// HUD draws, before its own panel (HudView.cs:164, "the bomber's sight first",
// 0x41e3c8):
//
//   BOMBTARG at the view's centre (300, 180), CROSS at the cursor (rounded), and
//   the bombs left right-aligned at x 472 on the baseline y 56 in the big font --
//   all in the 600×360 view, centred on the canvas.
//
// It draws through the canvas seam (render/render.h: mdk_render_canvas_quad), the
// path every HUD image takes on both frontends; the 3DS backend sets the canvas
// pass's GPU state for each quad itself (renderer_c3d's canvas path), so the
// overlay adds no state of its own.
// ============================================================================

#include <stdint.h>

#include <string>

#include "render/render.h"

namespace mdk {

class FontView;
class ModImages;

namespace formats {
class Bni;
class Palette;
} // namespace formats

class BomberOverlay {
public:
    // C# BomberOverlay.Target / Cross (BomberOverlay.cs:20-21): the sprites' names.
    static const char* const kTarget;
    static const char* const kCross;

    // C# BomberOverlay's constructor: BOMBTARG's and CROSS's frame 0 as index
    // textures through the HUD's palette (`palette`, its bytes `colours`). False when
    // either sprite is missing. `targetHd` / `crossHd`: the 3DS HD pack's file for
    // each picture (hdPaths below), which is then its only copy with HD on
    // (render.h, mdk_render_create_hd_image); null or empty: none.
    bool load(MdkRenderer renderer, const formats::Bni& sprites, int32_t palette,
              const formats::Palette& colours, const char* targetHd = nullptr,
              const char* crossHd = nullptr);
    // The 3DS HD pack's files for BOMBTARG_0 and CROSS_0 (a compressed ModImages),
    // for load. Empty where there is none. In bomber_overlay_mods.cpp.
    static void hdPaths(const formats::Bni& sprites, const formats::Palette& colours,
                        ModImages* mods, std::string* target, std::string* cross);
    // The constructor's CanvasImages.Replace: a mod's image (CanvasImages.FrameName:
    // BOMBTARG_0, CROSS_0) replaces each picture. In game/bomber_overlay_mods.cpp, so
    // the goldens' binary (tests/scripts) links the overlay without the mods.
    void replaceWithMods(MdkRenderer renderer, const formats::Bni& sprites,
                         const formats::Palette& colours, ModImages* mods);
    bool ready() const { return target_.texture >= 0 && cross_.texture >= 0; }

    // C# BomberOverlay.Draw (BomberOverlay.cs:59-66) on a canvas `canvasWidth` wide,
    // the count in `font` (the HUD's big font; null before the fonts are ready: the
    // count is not drawn, as the HUD's messages are not).
    void draw(MdkRenderer renderer, const FontView* font, float canvasWidth, float cursorX,
              float cursorY, int32_t bombs) const;

private:
    // C# BomberOverlay.Image: a texture, its size and its frame's hotspot.
    struct Image {
        int32_t texture = -1;
        float w = 0.0f;
        float h = 0.0f;
        float hotspotX = 0.0f;
        float hotspotY = 0.0f;
    };

    bool loadImage(MdkRenderer renderer, const formats::Bni& sprites, const char* animation,
                   const formats::Palette& colours, const char* hdPath, Image* out);
    void replaceWithMod(MdkRenderer renderer, const formats::Bni& sprites, const char* animation,
                        const formats::Palette& colours, ModImages* mods, const Image& image);
    void drawImage(MdkRenderer renderer, const Image& image, float atX, float atY) const;

    int32_t palette_ = -1;
    Image target_;
    Image cross_;
};

} // namespace mdk
