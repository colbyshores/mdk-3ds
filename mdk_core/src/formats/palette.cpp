// ============================================================================
// palette.cpp — see palette.h.
// ============================================================================

#include "formats/palette.h"

#include <string.h>

namespace mdk {
namespace formats {

Palette::Palette() {
    memset(rgba_, 0, sizeof(rgba_));
}

Palette Palette::fromRgb(const uint8_t* rgb, int32_t byteCount) {
    Palette palette;
    const int32_t count = (byteCount / 3 < kSize) ? (byteCount / 3) : kSize;
    for (int32_t i = 0; i < count; i++) {
        palette.rgba_[i * 4 + 0] = rgb[i * 3 + 0];
        palette.rgba_[i * 4 + 1] = rgb[i * 3 + 1];
        palette.rgba_[i * 4 + 2] = rgb[i * 3 + 2];
        palette.rgba_[i * 4 + 3] = 255;
    }

    // The game forces index 0 to black; sprites treat it as transparent.
    palette.rgba_[0] = 0;
    palette.rgba_[1] = 0;
    palette.rgba_[2] = 0;
    return palette;
}

Palette Palette::withArenaColors(const uint8_t* arenaRgb, int32_t byteCount) const {
    Palette palette;
    memcpy(palette.rgba_, rgba_, sizeof(palette.rgba_));
    const int32_t count = byteCount / 3;
    for (int32_t i = 0; i < count; i++) {
        const int32_t index = kArenaFirstIndex + i;
        if (index >= kSize) {
            break;
        }
        palette.rgba_[index * 4 + 0] = arenaRgb[i * 3 + 0];
        palette.rgba_[index * 4 + 1] = arenaRgb[i * 3 + 1];
        palette.rgba_[index * 4 + 2] = arenaRgb[i * 3 + 2];
    }
    return palette;
}

Palette Palette::withSystemColors(const uint8_t* systemRgb, int32_t byteCount) const {
    Palette palette;
    memcpy(palette.rgba_, rgba_, sizeof(palette.rgba_));
    // The loop starts at 1: index 0 stays black, because the game forces it
    // to black and the sprites treat it as transparent.
    int32_t count = byteCount / 3;
    if (count > kArenaFirstIndex) {
        count = kArenaFirstIndex;
    }
    for (int32_t i = 1; i < count; i++) {
        palette.rgba_[i * 4 + 0] = systemRgb[i * 3 + 0];
        palette.rgba_[i * 4 + 1] = systemRgb[i * 3 + 1];
        palette.rgba_[i * 4 + 2] = systemRgb[i * 3 + 2];
    }
    return palette;
}

} // namespace formats
} // namespace mdk
