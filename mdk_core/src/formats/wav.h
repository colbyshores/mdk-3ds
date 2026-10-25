#pragma once

// ============================================================================
// wav.h — PCM sound from a RIFF WAV.
//
// THE C# ORIGINAL is src/Mdk.Formats/Wav.cs. This is a 1:1 port.
//
// ONLY THE `fmt ` AND `data` CHUNKS ARE READ. The chunk walk is tolerant on
// purpose, because MDK's own sound files are not all well formed:
//
//   * GATTFIRE miscounts a pad byte, so its chunk sizes do not add up;
//   * some files truncate their LIST chunks.
//
// So a chunk's size is CLAMPED to what the file actually holds, and the walk
// continues from the clamped size. A file whose chunks are consistent (the
// common case) is read exactly the same.
//
// THE DATA IS COPIED. The C#'s `wav.AsSpan(..).ToArray()` copies too, so
// the Wav owns its samples and outlives the buffer it was read from.
// ============================================================================

#include <stdint.h>

#include <vector>

namespace mdk {
namespace formats {

// PCM sound from a RIFF WAV.
class Wav {
public:
    int32_t channels() const { return channels_; }
    int32_t sampleRate() const { return sampleRate_; }
    int32_t bitsPerSample() const { return bitsPerSample_; }

    // The samples, `dataSize()` bytes.
    const uint8_t* data() const { return data_.data(); }
    int32_t dataSize() const { return (int32_t)data_.size(); }

    // Samples per channel: what Sound.FromPcm makes of the chunk (8-bit is one
    // byte per sample, anything else two, over the even part of the data), then
    // divided by the channels as Sound.Frames does (AudioDevice.cs:14). Zero
    // channels give zero.
    int32_t frames() const;

    // Parses a WAV. FALSE when it has no `fmt ` chunk, no `data` chunk, or a
    // `fmt ` chunk too short to hold the header fields: the C# returns null
    // for the first two and throws for the third.
    static bool parse(const uint8_t* wav, int32_t byteCount, Wav* outWav);

private:
    int32_t channels_ = 0;
    int32_t sampleRate_ = 0;
    int32_t bitsPerSample_ = 0;
    std::vector<uint8_t> data_;
};

} // namespace formats
} // namespace mdk
