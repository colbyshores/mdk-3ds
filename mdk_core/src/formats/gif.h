#pragma once

// ============================================================================
// gif.h — a still GIF (MISC/MDKS_00n.GIF, the menu's slideshow).
//
// THE C# ORIGINAL is src/Mdk.Formats/Gif.cs. This is a 1:1 port.
//
// THE STRUCTURE:
//   header ─► global palette ─► extensions (skipped) ─► image descriptor ─►
//   LZW data
//
// Only the FIRST image is read: the menu's slides are stills, and the C#
// stops at the first image descriptor.
//
// THE PALETTE IS THE WHOLE COLOUR SYSTEM HERE: the slide's indices are
// palette indices and the 256 RGB entries come from the file's colour table.
// A file with a smaller table leaves the rest of the palette black.
//
// THE IMAGE IS PLACED IN THE SCREEN'S GRID, not in a buffer of its own: the
// header's width and height are the screen's, and the image descriptor's
// left and top place the picture inside it. A row past the bottom is
// dropped, and a row that would not fit is clipped to the screen's width.
//
// THE LZW DECODER is the standard variable-width one, with the GIF's
// twist that a code may be one the table does not have yet: it is then the
// previous string plus its own first byte.
// ============================================================================

#include <stdint.h>

#include <vector>

namespace mdk {
namespace formats {

// A still GIF.
class Gif {
public:
    int32_t width() const { return width_; }
    int32_t height() const { return height_; }

    // The picture's palette indices, `width() * height()` bytes.
    const uint8_t* indices() const { return indices_.data(); }
    int32_t indexCount() const { return (int32_t)indices_.size(); }

    // The palette, 256 RGB entries.
    const uint8_t* palette() const { return palette_.data(); }

    // Parses a GIF. FALSE when the file is not one, or when its image
    // descriptor does not fit it: the C#'s Load returns null in both cases
    // (the second through an exception from Span.CopyTo).
    static bool load(const uint8_t* bytes, int32_t byteCount, Gif* outGif);

private:
    int32_t width_ = 0;
    int32_t height_ = 0;
    std::vector<uint8_t> indices_;
    std::vector<uint8_t> palette_;
};

} // namespace formats
} // namespace mdk
