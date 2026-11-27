#pragma once

// ============================================================================
// texture_archive.h — an arena's or a level's texture archive.
//
// A 1:1 port of src/Mdk.Formats/TextureArchive.cs.
//
// THE FORMAT: char[12] name, u32 size, u32 count, then per entry:
//     char[8] name, u32 kind, u32 value, f32 ?, u32 offset (from the base)
//
// The KIND says what the entry is:
//
//   0xFFFFFFFF         a palette colour; VALUE is the index (256 and up are
//                      special values, not real indices)
//   high 16 bits set   an animated texture; the high bits carry the frame
//                      count
//   bit 0 set          the texture is uploaded WITH ALPHA: index 0 is
//                      see-through
//
// THE ZERO RULE. Palette index 0 is see-through everywhere for the sprites,
// but a world texture that paints with index 0 (LEVEL8's walls, I2_WALL1)
// would become a hole in the world. So for a NON-keyed texture, index 0 is
// REPLACED with palette entry 16 (BLACK) at parse time. The replacement
// happens HERE, in the parser, not at draw time: the C# does the same, and
// doing it later would change what the game reads.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

#include "formats/texture.h"

namespace mdk {
namespace formats {

// A texture archive.
class TextureArchive {
public:
    // Standalone files start with a size word.
    static const int32_t kFileBase = 4;

    // The name and size before the count. The 1996 demo's archives have
    // none.
    static const int32_t kHeader = 16;

    // How the archive's names are spelled.
    enum class Names {
        AsStored,   // the port's archives are in upper case
        Upper,      // the 1996 demo's are in lower case
    };

    // Palette index 0: see-through everywhere, or only in the keyed
    // textures.
    enum class Zero {
        SeeThrough,
        ByKind,
    };

    TextureArchive() = default;

    // The archive whose count is `header` bytes after `baseOffset`.
    static TextureArchive parse(const uint8_t* bytes, int32_t byteCount, int32_t baseOffset,
                                int32_t header, Names names, Zero zero = Zero::SeeThrough);

    // The ordinary archive: a 16-byte header, the names as stored, and the
    // zero rule by kind.
    static TextureArchive parse(const uint8_t* bytes, int32_t byteCount, int32_t baseOffset);

    // The textures, by name.
    const std::vector<std::pair<std::string, Texture>>& textures() const { return textures_; }

    // The textures, by name, for a caller that changes them. The 1996
    // demo's walls paint with palette index 0 as black, and the change is
    // made to the texture itself.
    std::vector<std::pair<std::string, Texture>>& mutableTextures() { return textures_; }

    // The palette colours, by name. The value is a palette index, or 256
    // and up for a special value.
    const std::vector<std::pair<std::string, int32_t>>& colors() const { return colors_; }

    // The texture with this name, or null.
    const Texture* findTexture(const std::string& name) const;

    // The palette index for this colour name, or -1.
    int32_t findColor(const std::string& name) const;

    // Adds a texture, or replaces one of the same name. The 1996 demo's
    // loose effect textures are added to the level's archive this way.
    void add(const std::string& name, const Texture& texture);

private:
    std::vector<std::pair<std::string, Texture>> textures_;
    std::vector<std::pair<std::string, int32_t>> colors_;
    // findTexture's index: the entries' positions, sorted by name (stably, so
    // a name's first entry comes first -- the scan's answer). Made by parse()
    // and add(), at load, in one allocation; remade by a lookup only if the
    // entry count changed behind it (names are never changed in place:
    // mutableTextures changes textures, add() appends or keeps the name).
    // Lookups are main-thread only.
    void index() const;
    mutable std::vector<uint32_t> byName_;
    mutable size_t indexed_ = (size_t)-1;
};

} // namespace formats
} // namespace mdk
