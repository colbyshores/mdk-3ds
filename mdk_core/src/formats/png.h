#pragma once

// ============================================================================
// png.h — PNG images of 8-bit channels, not interlaced.
//
// THE C# ORIGINAL is src/Mdk.Formats/Png.cs. This is a 1:1 port.
//
// WHAT IS READ: grey, grey and alpha, RGB or RGBA, every row filter, always
// into RGBA8.
//
// WHAT IS WRITTEN: RGB or RGBA, the rows unfiltered.
//
// THE STRUCTURE:
//   signature, IHDR (width, height, depth 8, colour type), IDAT... (zlib:
//   per row a filter byte, then the row), IEND; each chunk: u32 length,
//   type, data, CRC-32 (big-endian)
//
// THE ERRORS ARE REPORTED, NOT THROWN. The C# throws InvalidDataException
// in six places; here each of them is a false return with a message, so the
// caller can tell a corrupt file from a truncated one. The messages are the
// C#'s own.
//
// THE ZLIB DEPENDENCY. The C# uses System.IO.Compression's ZLibStream. The
// C++ uses zlib itself, which devkitPro ships for the 3DS
// (portlibs/3ds/lib/libz.a) and every desktop has. The build needs `-lz`.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

namespace mdk {
namespace formats {

// PNG images.
class Png {
public:
    // The channels written.
    enum class Channels {
        Rgb,
        Rgba,
    };

    // A decoded picture, always RGBA8.
    struct Image {
        int32_t width = 0;
        int32_t height = 0;
        std::vector<uint8_t> rgba;
    };

    // Decodes a PNG. FALSE when it is not one, or when it is broken; the
    // reason is in `outError` when that is wanted.
    static bool decode(const uint8_t* bytes, int32_t byteCount, Image* outImage, std::string* outError);

    // Encodes an RGBA8 image as a PNG of `channels` (RGB drops the alpha).
    // FALSE when `rgba` is too short for the picture.
    static bool encode(int32_t width, int32_t height, const uint8_t* rgba, int32_t byteCount,
                       Channels channels, std::vector<uint8_t>* outBytes, std::string* outError);

private:
    // The RGBA8 of a decoded picture.
    static bool toRgba(const std::vector<uint8_t>& raw, int32_t width, int32_t height, int32_t bpp,
                       std::vector<uint8_t>* outRgba);
};

} // namespace formats
} // namespace mdk
