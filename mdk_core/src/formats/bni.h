#pragma once

// ============================================================================
// bni.h — a BNI archive: TRAVSPRT.BNI, OPTIONS.BNI, STATS.BNI.
//
// A 1:1 port of src/Mdk.Formats/Bni.cs.
//
// THE FILE is an archive index with TWELVE-CHARACTER names: the images and
// the sprite animations of the game's sprites, the options menu's panel, the
// statistics screen.
//
// AN IMAGE ENTRY is a plain image: `u16 width, u16 height`, then the
// palette indices.
//
// A SPRITE ANIMATION ENTRY is a `u32 size` and then the animation, so the
// animation starts four bytes into the entry.
//
// THE ARCHIVE CANNOT BE COPIED. The sprite animations it hands out read its
// bytes when their frames are decoded, which happens on first use, and a
// copy of the archive would have its own bytes while the animations still
// pointed at the original's. The C# has no such problem because a C# class
// is a reference and a copy of the reference shares the array.
// ============================================================================

#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

#include "formats/fti.h"
#include "formats/sprite_animation.h"
#include "formats/texture.h"

namespace mdk {
namespace formats {

// A BNI archive.
class Bni {
public:
    // Read a BNI. The bytes are copied. Returns false when the data runs
    // out.
    static bool parse(Bni* out, const uint8_t* bytes, int32_t byteCount);

    // Read a BNI, taking the caller's buffer over. No copy.
    static bool parse(Bni* out, std::vector<uint8_t>&& bytes);

    // The default constructor: parse() takes the object as an out
    // parameter, so one has to be creatable.
    Bni() = default;

    Bni(const Bni&) = delete;
    Bni& operator=(const Bni&) = delete;
    Bni(Bni&&) = default;
    Bni& operator=(Bni&&) = default;

    const std::vector<uint8_t>& bytes() const { return index_.bytes(); }

    const ArchiveIndex& index() const { return index_; }

    // The entries, by offset.
    const std::vector<std::pair<std::string, ArchiveIndex::Entry>>& entries() const { return index_.entries(); }

    bool has(const std::string& name) const;

    // Adds or replaces an entry (the 1996 demo's loose sprite files).
    //
    // THE BYTES MOVE, so the images and the animations obtained before must
    // not be used after this.
    void add(const std::string& name, const uint8_t* data, int32_t size);

    // A plain image entry (u16 width, u16 height, palette indices). Returns
    // false when there is no such entry.
    bool image(const std::string& name, Texture* out) const;

    // A sprite animation entry (u32 size, then the animation). Returns false
    // when there is no such entry.
    bool animation(const std::string& name, SpriteAnimation* out) const;

private:
    ArchiveIndex index_;

    // The images and the animations decoded so far. The C# caches them in
    // dictionaries, for the same reason: a sprite is drawn every frame and
    // its animation is decoded once.
    mutable std::vector<std::pair<std::string, Texture>> images_;
    mutable std::vector<std::pair<std::string, SpriteAnimation>> animations_;
};

} // namespace formats
} // namespace mdk
