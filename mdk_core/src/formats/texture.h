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
// THE INDICES ARE NOT A COPY for the plain form -- they point into the file
// buffer. The caller owns that buffer. The animated form DOES copy, because
// the delta form has to be decoded into a new buffer.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

namespace mdk {
namespace formats {

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
    // bytes.
    const uint8_t* indices() const { return indices_.data(); }
    uint8_t* mutableIndices() { return indices_.data(); }
    int32_t indexCount() const { return (int32_t)indices_.size(); }

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
};

} // namespace formats
} // namespace mdk
