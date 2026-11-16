#pragma once

// ============================================================================
// font_view.h -- draws a font of MDKFONT.FTI like the original
// (0x415a20): glyphs from `ascent` rows above the baseline to `descent`
// below, scaled about the baseline (0x415d8c). A port of
// src/Mdk.Game/Hud/FontView.cs with one layout change: the oracle's
// atlas is a single row of glyphs (~4400 px for FONTBIG), which no
// PICA200 texture can hold (1024 px a side), so the glyphs wrap into
// rows of at most kAtlasRowWidth. The drawing math is the oracle's;
// only the atlas packing differs, and the mod atlas replacement that
// assumes the single row is not ported (mods are the P6 (g) slice).
// ============================================================================

#include <stdint.h>

#include "formats/font.h"
#include "render/render.h"
#include "render/render_types.h"

namespace mdk {

class FontView {
public:
    // Build the atlas and upload it as an index texture through
    // `palette` (a palette id from mdk_render_create_palette; its RGBA
    // is also handed to the texture, which the 3DS backend needs).
    bool load(MdkRenderer renderer, const formats::Font& font, int32_t palette,
              const uint8_t* paletteRgba);

    bool ready() const { return texture_ >= 0; }

    // Width of a text in pixels, at scale 1.
    int32_t width(const char* text) const { return font_.width(std::string(text)); }

    // Draw a text from `x` along the baseline `y`, scaled about the
    // baseline, in `tint`. Returns the x after it.
    float draw(MdkRenderer renderer, const char* text, float x, float y, float scale,
               const Colour& tint) const;

private:
    formats::Font font_;
    int32_t texture_ = -1;
    int32_t palette_ = -1;
    int32_t x_[256] = {};
    int32_t y_[256] = {};
    int32_t maxAscent_ = 0;
    float atlasWidth_ = 0.0f;
    float atlasHeight_ = 0.0f;
};

} // namespace mdk
