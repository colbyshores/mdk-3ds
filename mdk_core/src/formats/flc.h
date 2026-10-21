#pragma once

// ============================================================================
// flc.h — an Autodesk FLC animation (MISC/FLIC/*.FLC).
//
// THE C# ORIGINAL is src/Mdk.Formats/Flc.cs. This is a 1:1 port.
//
// THE STRUCTURE:
//   header (128) ─► frame 1 (palette + BYTE_RUN) ─► frames 2..n (DELTA_FLC) ─►
//   ring frame (to frame 1)
//
// The file has ONE MORE FRAME than it shows: the last one is the ring frame
// that returns to the first. `frameCount()` is the number to SHOW, and
// `nextFrame()` stops after that many.
//
// THE FRAMES ARE DECODED ONE AT A TIME into the same buffer, the way the
// C# does: an FLC frame is usually a DELTA_FLC, a list of changes to the
// frame before it, so the picture has to be built up in place.
//
// THE HEADER'S MAGIC IS AT OFFSET 4, not 0: an FLC starts with the file's
// size. The C# checks `Bin.U16(bytes, 4) != 0xAF12`, and so does this.
//
// EVERY READ IS CHECKED. The C# indexes its byte array directly and throws
// when a chunk lies about its size; a C++ that read past the end would be
// undefined behaviour, so a broken file is reported as Broken instead.
// ============================================================================

#include <stdint.h>

#include <vector>

namespace mdk {
namespace formats {

// An Autodesk FLC animation.
class Flc {
public:
    // What a frame step did. The C#'s NextFrame() returns false at the end
    // of the movie and throws on broken data; this tells the two apart.
    enum class FrameStatus {
        Decoded,
        End,
        Broken,
    };

    // Parses a file's header. FALSE when it is not an FLC.
    static bool load(const uint8_t* bytes, int32_t byteCount, Flc* outFlc);

    int32_t width() const { return width_; }
    int32_t height() const { return height_; }
    // The frames to SHOW; the file has one more, the ring frame.
    int32_t frameCount() const { return frameCount_; }
    // Milliseconds per frame.
    int32_t speed() const { return speed_; }

    // The current frame's palette indices, width() * height() bytes.
    const uint8_t* indices() const { return indices_.data(); }
    int32_t indexCount() const { return (int32_t)indices_.size(); }

    // The palette, 256 RGB entries.
    const uint8_t* palette() const { return palette_.data(); }

    // The frames decoded since the start.
    int32_t frame() const { return frame_; }

    // Back to before the first frame.
    void rewind();

    // Decodes the next frame.
    FrameStatus nextFrame();

private:
    bool decodeChunk(uint16_t type, int32_t at);
    bool decodePalette(int32_t at, int32_t scale);
    bool decodeByteRun(int32_t at);
    bool decodeDeltaFlc(int32_t at);
    bool decodeDeltaFli(int32_t at);

    std::vector<uint8_t> bytes_;
    std::vector<uint8_t> indices_;
    std::vector<uint8_t> palette_;
    int32_t firstFrame_ = 0;
    int32_t pos_ = 0;
    int32_t width_ = 0;
    int32_t height_ = 0;
    int32_t frameCount_ = 0;
    int32_t speed_ = 0;
    int32_t frame_ = 0;
};

} // namespace formats
} // namespace mdk
