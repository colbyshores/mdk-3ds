#pragma once

// ============================================================================
// mve.h — an Interplay MVE movie (MISC/FLIC/MDKBZK.MVE, the end of the game).
//
// THE C# ORIGINAL is src/Mdk.Formats/Mve.cs. This is a 1:1 port.
//
// THE STRUCTURE:
//   signature (26) ─► chunk [u16 size, u16 type] ─► opcode [u16 size, u8 type,
//   u8 version, data]
//
// The opcodes set up the screen and the sound, and then each frame is a
// decoding map (4 bits per 8x8 block), the blocks' data (video format 0x11,
// 8 bits) and DPCM sound.
//
// THE FRAMES DECODE INTO THE BACK BUFFER, which still holds the frame before
// the last: the movie is DOUBLE BUFFERED, and a block may stay as it is, be
// copied from the frame shown last or from the back buffer, or be drawn from
// 1 to 64 colours. `nextFrame()` runs the opcodes up to the next shown
// frame.
//
// THE BLOCK CODES ARE NOT WHAT FFMPEG CALLS THEM. This decoder follows the
// original: the code picks how a block is stored (copied, solid, two or four
// colours), and the COLOUR ORDER in the file decides the layout, because
// "P0 <= P1" means the two colours are sorted and the block is a bit per
// pixel, while "P0 > P1" means a different layout entirely.
//
// THE PALETTE IS 6 BITS, scaled to 8 as ffmpeg does: v * 4 + v / 16.
// ============================================================================

#include <stdint.h>

#include <vector>

namespace mdk {
namespace formats {

// An Interplay MVE movie.
class Mve {
public:
    // What a frame step did. The C#'s NextFrame() returns false at the end
    // of the movie and throws on broken data; this tells the two apart.
    enum class FrameStatus {
        Decoded,
        End,
        Broken,
    };

    // One sound sample: the left and right values, -1 to 1.
    struct AudioSample {
        float left = 0.0f;
        float right = 0.0f;
    };

    // Parses a movie's header. FALSE when it is not an MVE.
    static bool load(const uint8_t* bytes, int32_t byteCount, Mve* outMve);

    int32_t width() const { return width_; }
    int32_t height() const { return height_; }
    // Microseconds per frame.
    int32_t frameTime() const { return frameTime_; }
    int32_t sampleRate() const { return sampleRate_; }
    int32_t channels() const { return channels_; }

    // The palette, 256 RGB entries.
    const uint8_t* palette() const { return palette_.data(); }

    // The sound decoded since the last call.
    const std::vector<AudioSample>& audio() const { return audio_; }

    // The frames decoded since the start.
    int32_t frame() const { return frame_; }

    // The frame shown last (palette indices).
    const uint8_t* indices() const { return last_.data(); }
    int32_t indexCount() const { return (int32_t)last_.size(); }

    // Runs the opcodes up to the next shown frame.
    FrameStatus nextFrame();

private:
    bool run(uint8_t type, uint8_t version, int32_t at, int32_t length);
    void initVideo(int32_t width, int32_t height);
    bool setPalette(int32_t at);
    bool decodeAudio(int32_t at, int32_t end, int32_t size);
    bool decodePcm(int32_t at, int32_t end);
    void pushSamples(const int32_t* samples, int32_t count);
    bool decodeVideo(int32_t at, int32_t end);
    bool decodeBlock(int32_t code, int32_t x, int32_t y, int32_t at, int32_t* outAt);
    void copy(int32_t at, int32_t dx, int32_t dy, bool fromLast);
    void fill(int32_t at, int32_t width, int32_t height, uint8_t value);
    bool twoColours(int32_t at, int32_t p, int32_t* outAt);
    bool twoColourQuadrants(int32_t at, int32_t p, int32_t* outAt);
    bool fourColours(int32_t at, int32_t p, int32_t* outAt);
    bool fourColourQuadrants(int32_t at, int32_t p, int32_t* outAt);

    std::vector<uint8_t> bytes_;
    std::vector<uint8_t> current_;
    std::vector<uint8_t> last_;
    std::vector<uint8_t> map_;
    std::vector<uint8_t> palette_;
    std::vector<AudioSample> audio_;
    int32_t predictors_[2] = {0, 0};
    int32_t pos_ = 0;
    int32_t width_ = 0;
    int32_t height_ = 0;
    int32_t frameTime_ = 0;
    int32_t sampleRate_ = 0;
    int32_t channels_ = 1;
    int32_t audioFlags_ = 0;
    int32_t frame_ = 0;
    bool shown_ = false;
    bool ended_ = false;
};

} // namespace formats
} // namespace mdk
