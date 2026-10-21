#pragma once

// ============================================================================
// palette.h — MDK's 256-colour palette.
//
// A 1:1 port of src/Mdk.Formats/Palette.cs.
//
// THE PALETTE IS THE WHOLE COLOUR SYSTEM. MDK's world textures are 8-bit
// INDEXED: a texture holds palette indices, and the palette turns them into
// colours. Three ranges, and the ranges are not interchangeable:
//
//   index 0        black, and TRANSPARENT for sprites
//   indices 1-63   the system colours (SYS_PAL, from MISC/mdkfont.fti)
//   indices 64-175 an arena's own 112 colours
//   indices 176-255 the level's base colours (the DTI's palette)
//
// The 3DS has no palette texture format and no programmable fragment
// shader, so the 3DS backend expands indices to RGBA on the CPU. The Vulkan
// backend does the lookup in the shader. Both produce the SAME colours; see
// SPEC.md 7.3.
// ============================================================================

#include <stdint.h>

namespace mdk {
namespace formats {

// A 256-colour palette for MDK's 8-bit textures.
class Palette {
public:
    static const int32_t kSize = 256;

    // The first index an arena's own colours replace.
    static const int32_t kArenaFirstIndex = 64;

    // The number of colours an arena brings.
    static const int32_t kArenaColorCount = 112;

    Palette();

    // The palette's RGBA8, four bytes per index.
    const uint8_t* rgba() const { return rgba_; }
    uint8_t* mutableRgba() { return rgba_; }

    // A palette from an RGB table. `rgb` is three bytes per colour.
    static Palette fromRgb(const uint8_t* rgb, int32_t byteCount);

    // A copy with an arena's 112 colours at indices 64-175.
    Palette withArenaColors(const uint8_t* arenaRgb, int32_t byteCount) const;

    // A copy with the system colours (SYS_PAL) at indices 1-63. Index 0
    // stays black.
    Palette withSystemColors(const uint8_t* systemRgb, int32_t byteCount) const;

private:
    uint8_t rgba_[kSize * 4];
};

} // namespace formats
} // namespace mdk
