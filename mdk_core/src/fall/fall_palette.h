#pragma once

// ============================================================================
// fall_palette.h — the fall's palette effects on FALLPn (0x5209c8, 0x410cc0).
//
// THE C# ORIGINAL is src/Mdk.Game/Fall/FallPalette.cs, ported operation for
// operation: the red of death raises the red of colours 1-254, then the
// brightness b whitens every component:
//
//   c' = (c × k + (256 − k) × 255) >> 8      k = round(256 × clamp(b, 0, 1))
//   b = 1: unchanged    b = 0.5: 0 → 127, 100 → 177    b = 0: white
//
// Colour 0 is whitened only in the fall's first second.
//
// HOW THE FRONTENDS SHOW IT (fall_view.h): the C# re-uploads the palette (a
// texture there) whenever `set` changes it. This port's renderer seam has no
// palette update, and the 3DS could not afford one (every texture the palette
// expanded would have to be expanded again, SPEC.md 7.3), so the view draws the
// SAME arithmetic over the frame instead: `k` as a white fill of alpha
// (256 − k) / 256 -- c (1 − a) + 255 a is the formula above before its floor --
// and the red as an additive fill of `add` / 255. `rgba()`, `colour()` and
// `apply()` are the C#'s, kept whole (the goldens pin them, and the backdrop
// and the HUD's palette read them).
//
// Cold (once a frame, 256 colours at most when it changes): AGENTS.md §2.
// No virtuals, no templates, no exceptions (AGENTS.md §10).
// ============================================================================

#include <stdint.h>

#include "render/render_types.h"

namespace mdk {
namespace fall {

class FallPalette {
public:
    // FallPalette.Zero: whether colour 0 is whitened too.
    enum Zero { Whitened, Kept };

    static constexpr int32_t kColours = 256;
    static constexpr int32_t kChannels = 4;
    static constexpr int32_t kUnit = 256;

    // C# FallPalette(byte[] rgba): the palette (256 RGBA colours) the effects
    // start from; the effects start unapplied (k 256, no red, colour 0 whitened).
    explicit FallPalette(const uint8_t* rgba);

    // FallPalette.Level: k of a brightness.
    static int32_t level(float brightness);
    // FallPalette.Whiten.
    static uint8_t whiten(uint8_t c, int32_t level);

    // FallPalette.Set: applies a brightness and a red (0-1, added to R); false
    // when nothing changed.
    bool set(float brightness, float red, Zero zero);

    // FallPalette.Rgba: the palette with the effects.
    const uint8_t* rgba() const { return rgba_; }
    // The palette the effects start from.
    const uint8_t* base() const { return base_; }
    // FallPalette.Colour: colour `index` with the effects (0-1).
    Colour colour(int32_t index) const;
    // FallPalette.Apply: a colour outside the palette (0-1, alpha kept) with the
    // effects of colours 1-254.
    Colour apply(const Colour& colour) const;

    // The applied tuple (C# _applied): k, the red added (0-255) and colour 0's rule.
    int32_t appliedLevel() const { return level_; }
    int32_t appliedRed() const { return red_; }
    Zero appliedZero() const { return zero_; }

private:
    uint8_t base_[kColours * kChannels];
    uint8_t rgba_[kColours * kChannels];
    int32_t level_ = kUnit;
    int32_t red_ = 0;
    Zero zero_ = Whitened;
};

} // namespace fall
} // namespace mdk
