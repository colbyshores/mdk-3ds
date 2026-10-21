#pragma once

// ============================================================================
// fall3d.h — the fall's own entries of FALL3D.BNI.
//
// THE C# ORIGINAL is src/Mdk.Formats/Fall3d.cs. This is a 1:1 port.
//
// TWO KINDS OF ENTRY:
//   ZOOMnnnn  u32 size, then 180 records (one per pair of screen rows):
//             u32 nL, u8 left[4 nL], u32 nM, u32 nR, u8 right[4 nR]
//             (nL + nM + nR = 150)
//   FALLPU_n  char[12] pickup model names, ended by a name starting with 0
//
// THE HAZE IS A BLEND TABLE, NOT A PICTURE. A byte per pixel says how far
// the row blends into the middle: 0 is plain, 1 to 8 are the offsets of the
// blend table. The three runs of every record are the left plain part, the
// middle part's width (the middle itself is not stored) and the right plain
// part.
//
// THE PICKUP NAMES ARE TWELVE BYTES, NUL-PADDED, and the list ends with a
// name that starts with a NUL.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

#include "formats/bni.h"

namespace mdk {
namespace formats {

// The fall's own entries of FALL3D.BNI.
class Fall3d {
public:
    // The haze frame's width.
    static const int32_t kHazeWidth = 600;

    // One record covers two screen rows of the 360-high view.
    static const int32_t kHazeRows = 180;

    // A haze frame as one byte per pixel (0 plain, 1-8 the blend table
    // offset), 600 x 180.
    //
    // FALSE when the archive has no such entry, or when its records do not
    // fit the frame: the C# throws there.
    static bool haze(const Bni& bni, const std::string& name, std::vector<uint8_t>* outPixels);

    // The pickups dropped in the fall of `name` (none if there is no such
    // entry).
    static std::vector<std::string> pickups(const Bni& bni, const std::string& name);
};

} // namespace formats
} // namespace mdk
