#pragma once

// ============================================================================
// font.h — a font of MISC/MDKFONT.FTI (FONTBIG, FONTSML).
//
// THE C# ORIGINAL is src/Mdk.Formats/Font.cs. This is a 1:1 port.
//
// THE FORMAT:
//   u32 glyph offsets[256] (from the font, 0 = none), then per glyph:
//     s8 ascent, s8 descent, u8 width, (ascent + descent + 1) x width palette
//     indices
//
// A glyph spans `ascent` rows above the baseline to `descent` below.
// Characters without a glyph are spaces. Pixels use the palette's first 64
// colours (SYS_PAL); 0 is transparent.
//
// THE SPACE WIDTH IS NOT IN THE FILE. It is 6 in FONTBIG and 4 in FONTSML,
// and the caller says which: the C#'s Font.Parse takes it as an argument,
// and so does this. A glyph's own width is used when the character has one.
//
// THE GLYPHS ARE COPIED OUT of the font's bytes. The C#'s `data.AsSpan(..)
// .ToArray()` does the same, so a Font outlives the buffer it was read
// from — which matters, because the FTI archive's bytes are the file's.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

namespace mdk {
namespace formats {

// A font of MISC/MDKFONT.FTI.
class Font {
public:
    // The characters a font has room for.
    static const int32_t kCharacterCount = 256;

    // One glyph: its metrics and its palette indices, row by row.
    struct Glyph {
        int32_t ascent = 0;
        int32_t descent = 0;
        int32_t width = 0;
        std::vector<uint8_t> indices;
    };

    // Parses a font. `spaceWidth` is 6 in FONTBIG, 4 in FONTSML.
    //
    // FALSE when the font is broken: an offset outside the file, or a glyph
    // whose size runs past the end. The C# throws in both cases
    // (IndexOutOfRangeException), so the caller there learns about it; here
    // the caller decides what to do with a broken font.
    static bool parse(const uint8_t* data, int32_t byteCount, int32_t spaceWidth, Font* outFont);

    // The glyph of a character, or null when it has none (it is a space).
    const Glyph* glyph(int32_t character) const;

    int32_t spaceWidth() const { return spaceWidth_; }

    // The width of a text in pixels.
    int32_t width(const uint8_t* text, int32_t length) const;
    int32_t width(const std::string& text) const;

private:
    // The glyphs, one per character, and which characters have one.
    std::vector<Glyph> glyphs_;
    std::vector<uint8_t> present_;
    int32_t spaceWidth_ = 0;
};

} // namespace formats
} // namespace mdk
