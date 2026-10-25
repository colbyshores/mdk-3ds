// ============================================================================
// wav.cpp — see wav.h for why the chunk walk is tolerant.
// ============================================================================

#include "formats/wav.h"

#include <string.h>

#include "formats/bin_reader.h"

namespace mdk {
namespace formats {

namespace {

const int32_t RIFF_HEADER = 12;
const int32_t CHUNK_HEADER = 8;
// The `fmt ` chunk's fields the C# reads: u16 format, u16 channels, u32 rate,
// u32 byte rate, u16 align, u16 bits. The bits are the last two bytes.
const int32_t FORMAT_SIZE = 16;

} // namespace

bool Wav::parse(const uint8_t* wav, int32_t byteCount, Wav* outWav) {
    if (!outWav) {
        return false;
    }

    const uint8_t* format = nullptr;
    int32_t formatSize = 0;
    const uint8_t* data = nullptr;
    int32_t dataSize = 0;

    int32_t pos = RIFF_HEADER;
    while (pos + CHUNK_HEADER <= byteCount) {
        char id[5] = {0};
        binAscii(wav, pos, 4, id);

        // A truncated chunk can claim more than the file holds, so the size
        // is clamped to what is left. The C# does the same with a Math.Min.
        uint32_t claimed = 0;
        memcpy(&claimed, wav + pos + 4, 4);
        const int32_t available = byteCount - pos - CHUNK_HEADER;
        const int32_t size = (int32_t)(claimed < (uint32_t)available ? claimed : (uint32_t)available);
        const int32_t end = pos + CHUNK_HEADER + size < byteCount ? pos + CHUNK_HEADER + size : byteCount;

        if (strcmp(id, "fmt ") == 0) {
            format = wav + pos + CHUNK_HEADER;
            formatSize = end - pos - CHUNK_HEADER;
        } else if (strcmp(id, "data") == 0) {
            data = wav + pos + CHUNK_HEADER;
            dataSize = end - pos - CHUNK_HEADER;
        }

        // Chunks are padded to an even size.
        pos += CHUNK_HEADER + size + (size & 1);
    }

    if (!format || !data) {
        return false;
    }

    // The C# reads the fields without checking the chunk is long enough, and
    // throws when it is not. Here it is a failure.
    if (formatSize < FORMAT_SIZE) {
        return false;
    }

    Wav result;
    result.channels_ = (int32_t)binU16(format, 2);
    result.sampleRate_ = (int32_t)binU32(format, 4);
    result.bitsPerSample_ = (int32_t)binU16(format, 14);
    result.data_.assign(data, data + dataSize);

    *outWav = result;
    return true;
}

int32_t Wav::frames() const {
    const int32_t samples = bitsPerSample_ == 8 ? dataSize() : (dataSize() & ~1) / 2;
    return channels_ > 0 ? samples / channels_ : 0;
}

} // namespace formats
} // namespace mdk
