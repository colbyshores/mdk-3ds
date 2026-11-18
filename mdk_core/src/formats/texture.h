#pragma once

// ============================================================================
// texture.h — an 8-bit paletted texture.
//
// A 1:1 port of src/Mdk.Formats/Texture.cs.
//
// THE FORMAT: u16 width, u16 height, then the palette indices row by row.
// ANIMATED textures keep their frames one after another, so the indices
// array holds `frameCount * width * height` bytes and the frame is selected
// by the UV, not by a separate array. That is why the original's texture
// upload is a single tall strip.
//
// THE DELTA FORM (kind 0x20000) stores the first frame and then the CHANGES
// that make each next one, because the explosion and slime animations are
// mostly the same frame with a few blocks changed. Decoding them is a
// column-store trick: the changes are stored as (start, runs) controls in
// units of 4 pixels.
//
// THE INDICES ARE A COPY in every form: the texture owns its pixels and the
// file buffer can go once parse returns.
//
// THE PIXELS CAN BE AWAY (arena streaming, docs/ARENA_STREAMING_SPEC.md Phase
// 2). An arena's textures are most of its bytes in the level's MTO, and the
// original keeps only ONE arena's in memory. So a texture's pixels can be
// dropped (dropPixels) while the object itself -- its name, size and frame
// count, and its ADDRESS, which the renderer's caches key on -- stays, and
// brought back by its owner (adoptPixels, from a fresh parse of the same file
// bytes). A reader that finds them away asks the owner for them through the
// texture's pixel source, so indices() never returns an evicted texture's
// nothing: the arena stream brings pixels in ahead of need, and this is the
// backstop that keeps a missed case correct rather than blank.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

namespace mdk {
namespace formats {

class Texture;

// Who brings a texture's pixels back: the owner of the arena the texture
// belongs to (game/arena_stream.h). A plain struct of a function pointer and
// its state, as formats/file_system.h is and for the same reason (AGENTS.md
// section 10: no virtual dispatch). The texture BORROWS it; it must outlive
// every texture that points at it.
struct TexturePixelSource {
    // Make `texture`'s pixels resident (adoptPixels), or leave them away.
    void (*load)(void* user, const Texture* texture) = nullptr;
    void* user = nullptr;
};

// An 8-bit paletted texture.
class Texture {
public:
    // The animated kind whose frames are CHANGES to the first one.
    static const uint32_t kDeltas = 0x20000;

    // The unit the delta runs are counted in.
    static const int32_t kDeltaUnit = 4;

    Texture() = default;

    // A plain texture at `offset` in `bytes`.
    static Texture parse(const std::string& name, const uint8_t* bytes, int32_t byteCount, int32_t offset);

    // An animated texture at `offset` in `bytes`.
    static Texture parseAnimated(const std::string& name, const uint8_t* bytes, int32_t byteCount,
                                 int32_t offset, uint32_t kind);

    const std::string& name() const { return name_; }
    int32_t width() const { return width_; }
    // The height of ONE frame.
    int32_t height() const { return height_; }
    int32_t frameCount() const { return frameCount_; }

    // The palette indices of all the frames, `frameCount * width * height`
    // bytes. Pixels that are away are asked for first (see the header).
    const uint8_t* indices() const {
        needPixels();
        return indices_.data();
    }
    uint8_t* mutableIndices() {
        needPixels();
        return indices_.data();
    }
    int32_t indexCount() const {
        needPixels();
        return (int32_t)indices_.size();
    }

    // THE STREAM'S HALF. Whether the pixels are away; drop them (the storage is
    // freed, the shape is kept); take them back from `fresh`, a new parse of
    // this same texture -- false, taking nothing, unless its width, height and
    // frame count are this texture's and it has pixels. The source is who
    // indices() asks when they are away; null means nobody (they stay away).
    bool pixelsAway() const { return away_; }
    void dropPixels();
    bool adoptPixels(Texture* fresh);
    void setPixelSource(const TexturePixelSource* source) { source_ = source; }
    // The bytes the pixels hold right now (0 while away): the stream's accounts.
    int32_t residentBytes() const { return (int32_t)indices_.size(); }

    // The indices of one frame, for a caller that wants them separately.
    const uint8_t* frame(int32_t index) const;

    // Replace every index-0 byte with `value`. The 1996 demo's arenas and
    // LEVEL8's walls paint with index 0, and the palette's index 0 is
    // transparent, so the texture has to name a different palette entry.
    void replaceIndex(uint8_t from, uint8_t to);

    // The setters. The DTI builds the sky panorama itself -- it is a strip
    // of the level file, not a texture with a header -- so a texture has to
    // be assemblable from its parts.
    void setName(const std::string& name) { name_ = name; }
    void setWidth(int32_t width) { width_ = width; }
    void setHeight(int32_t height) { height_ = height; }
    void setFrameCount(int32_t count) { frameCount_ = count; }

    // Point the indices at a run of bytes, copying them. The DTI's sky is
    // copied rather than borrowed, because the level file is released once
    // the level is parsed.
    void setIndices(const uint8_t* indices, int32_t count);

private:
    std::string name_;
    int32_t width_ = 0;
    int32_t height_ = 0;
    int32_t frameCount_ = 1;
    std::vector<uint8_t> indices_;

    // The pixels were dropped (not: the texture is empty). Set only by
    // dropPixels, cleared by adoptPixels and setIndices.
    bool away_ = false;
    const TexturePixelSource* source_ = nullptr;

    // Ask the source for pixels that are away. Const because every reader is:
    // the source reaches the texture through its owner, which holds it mutable.
    void needPixels() const {
        if (away_ && source_ && source_->load) {
            source_->load(source_->user, this);
        }
    }
};

} // namespace formats
} // namespace mdk
