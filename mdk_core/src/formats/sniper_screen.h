#pragma once

// ============================================================================
// sniper_screen.h — sniper mode's screen, in TRAVSPRT.BNI.
//
// THE C# ORIGINAL is src/Mdk.Formats/SniperScreen.cs. This is a 1:1 port.
//
// TWO ENTRIES of the sprite archive:
//   SNIPERS1  the 640x480 frame, palette indices with no header
//   SNIPERS2  the mask over the 600x360 view, with holes for the scope and
//             the round cameras
//
// THE MASK'S WORDS. A word is either
//   0x8000..  skip that many transparent pixels
//   0xFF00    the end
//   0xFF01..  that many literal indices
//   below     that many literal indices, four to a word
//
// THE FRAME HAS NO SIZE FIELD AND THE MASK HAS ONE. The C# adds the four
// bytes for the mask only, and so does this: the two entries were written by
// different hands.
//
// PALETTE INDEX 0 IS TRANSPARENT, which is what makes the scope's hole.
// ============================================================================

#include <stdint.h>

#include <vector>

#include "formats/bni.h"
#include "formats/texture.h"

namespace mdk {
namespace formats {

// Sniper mode's screen.
class SniperScreen {
public:
    // The frame.
    static const int32_t kScreenWidth = 640;
    static const int32_t kScreenHeight = 480;

    // The view the mask covers.
    static const int32_t kViewWidth = 600;
    static const int32_t kViewHeight = 360;

    // The 640x480 frame's palette indices.
    //
    // FALSE when the archive has no SNIPERS1, or when it is too short: the
    // C# throws there (KeyNotFoundException, ArgumentOutOfRangeException).
    static bool frame(const Bni& sprites, Texture* outTexture);

    // The 600x360 mask's palette indices, 0 in its holes.
    static bool mask(const Bni& sprites, Texture* outTexture);

    // The mask's words, from `offset`. FALSE when they run past the end:
    // the C# throws there.
    static bool decodeMask(const uint8_t* bytes, int32_t byteCount, int32_t offset,
                           std::vector<uint8_t>* outIndices);

private:
    // The entry of the sprite archive, or null.
    static const ArchiveIndex::Entry* find(const Bni& sprites, const char* name);
};

} // namespace formats
} // namespace mdk
