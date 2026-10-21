#pragma once

// ============================================================================
// sprite_animation.h — an RLE sprite animation.
//
// A 1:1 port of src/Mdk.Formats/SpriteAnimation.cs.
//
// THE FILE:
//
//   u32 frame count
//   u32 frame offset[count]      (from the count)
//   per frame:
//     u16 width, u16 height, s16 hotspot x, s16 hotspot y
//     rows of commands:
//       0x00-0x7F  n + 1 literal indices follow
//       0x80-0xFD  the next index repeated n - 0x7C times
//       0xFE       end of row (the rest of the row is transparent)
//       0xFF       end of frame
//
// Palette index 0 is transparent.
//
// THE 0xFF COMMAND ENDS THE ROW, NOT THE FRAME. The C# breaks out of the
// row loop on any command of 0xFE or above, so 0xFE and 0xFF are the same
// thing to it, and the next row starts where the row ended. That is what
// the game shows, so that is what this does.
//
// THE ROW MAY BE SHORTER THAN THE WIDTH. A row's commands end before the
// width is used up and the rest of the row stays zero, which is how the
// transparent parts of a sprite are stored.
//
// THE FILE IS BORROWED. The frames are decoded on first use, so the buffer
// must outlive the animation.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

#include "formats/texture.h"

namespace mdk {
namespace formats {

// An RLE sprite animation, such as Kurt's K_RUN in TRAVSPRT.BNI.
class SpriteAnimation {
public:
    // A frame and its anchor. Kurt's top-left corner is drawn at
    // (feet x - hotspot x, feet y - 101 - hotspot y) on a 600x360 view.
    struct Frame {
        Texture image;
        int32_t hotspotX = 0;
        int32_t hotspotY = 0;
    };

    // Parse a sprite animation. Returns false when the data runs out.
    static bool parse(SpriteAnimation* out, const std::string& name, const uint8_t* bytes, int32_t byteCount,
                      int32_t offset);

    const std::string& name() const { return name_; }
    int32_t frameCount() const { return frameCount_; }

    // The frame at `index`, decoded on first use. Returns false when the
    // index is out of range or the data runs out.
    bool frame(int32_t index, Frame* out) const;

private:
    // Decode the frame at `offset`.
    bool decode(int32_t offset, Frame* out) const;

    std::string name_;

    // The file the frames are read from. BORROWED: the caller owns it and
    // it must outlive the animation.
    const uint8_t* bytes_ = nullptr;
    int32_t byteCount_ = 0;

    // The offset of the animation in the file: the frame count is there and
    // the frame offsets follow it.
    int32_t base_ = 0;

    int32_t frameCount_ = 0;

    // The frames decoded so far, and which ones they are.
    mutable std::vector<Frame> frames_;
    mutable std::vector<bool> decoded_;
};

} // namespace formats
} // namespace mdk
