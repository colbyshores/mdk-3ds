// fall_palette.cpp — see fall_palette.h. The C#'s FallPalette.cs, method for method.

#include "fall/fall_palette.h"

#include <math.h>
#include <string.h>

namespace mdk {
namespace fall {

namespace {

constexpr int32_t kShift = 8;
constexpr int32_t kLastReddened = 254;
constexpr int32_t kByteMax = 255;

float clampUnit(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

} // namespace

FallPalette::FallPalette(const uint8_t* rgba) {
    memcpy(base_, rgba, sizeof(base_));
    memcpy(rgba_, rgba, sizeof(rgba_));
}

int32_t FallPalette::level(float brightness) {
    // MathF.Round is round-half-to-even; rintf in the default rounding mode is too.
    return (int32_t)rintf((float)kUnit * clampUnit(brightness));
}

uint8_t FallPalette::whiten(uint8_t c, int32_t level) {
    return (uint8_t)(((int32_t)c * level + (kUnit - level) * kByteMax) >> kShift);
}

bool FallPalette::set(float brightness, float red, Zero zero) {
    // C# Set: `(Level(brightness), (int)MathF.Round(red * byte.MaxValue), zero)`.
    const int32_t level = FallPalette::level(brightness);
    const int32_t add = (int32_t)rintf(red * (float)kByteMax);
    if (level == level_ && add == red_ && zero == zero_) {
        return false;
    }

    level_ = level;
    red_ = add;
    zero_ = zero;
    const int32_t first = zero == Kept ? 1 : 0;
    memcpy(rgba_, base_, sizeof(rgba_));
    for (int32_t i = first; i < kColours; i++) {
        uint8_t* at = rgba_ + i * kChannels;
        if (i > 0 && i <= kLastReddened) {
            const int32_t r = at[0] + add;
            at[0] = (uint8_t)(r < kByteMax ? r : kByteMax);
        }

        for (int32_t c = 0; c < kChannels - 1; c++) {
            at[c] = whiten(at[c], level);
        }
    }

    return true;
}

Colour FallPalette::colour(int32_t index) const {
    // C# `new Vector4(r, g, b, 255) / 255`.
    const uint8_t* at = rgba_ + index * kChannels;
    Colour c;
    c.r = (float)at[0] / (float)kByteMax;
    c.g = (float)at[1] / (float)kByteMax;
    c.b = (float)at[2] / (float)kByteMax;
    c.a = 1.0f;
    return c;
}

Colour FallPalette::apply(const Colour& colour) const {
    // C# Apply: `red = Min(X + add / 255, 1)`, then `rgb * keep + white`.
    const float red = fminf(colour.r + (float)red_ / (float)kByteMax, 1.0f);
    const float keep = (float)level_ / (float)kUnit;
    const float white = (float)(kUnit - level_) / (float)kUnit;
    Colour c;
    c.r = red * keep + white;
    c.g = colour.g * keep + white;
    c.b = colour.b * keep + white;
    c.a = colour.a;
    return c;
}

} // namespace fall
} // namespace mdk
