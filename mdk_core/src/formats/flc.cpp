// ============================================================================
// flc.cpp — see flc.h for the structure and why every read is checked.
// ============================================================================

#include "formats/flc.h"

#include <string.h>

#include "formats/bin_reader.h"

namespace mdk {
namespace formats {

namespace {

const uint16_t MAGIC = 0xAF12;
const int32_t HEADER_SIZE = 128;
const int32_t FRAME_HEADER_SIZE = 16;
const int32_t CHUNK_HEADER_SIZE = 6;
const int32_t PALETTE_BYTES = 768;
const int32_t FIRST_FRAME_OFFSET = 80;

// The chunk types: frames; in a frame the palette (256 or 64 levels),
// deltas (word FLC, byte FLI), all black, a full run-length frame, raw
// pixels.
const uint16_t CHUNK_COLOR_256 = 4;
const uint16_t CHUNK_DELTA_FLC = 7;
const uint16_t CHUNK_COLOR_64 = 11;
const uint16_t CHUNK_DELTA_FLI = 12;
const uint16_t CHUNK_BLACK = 13;
const uint16_t CHUNK_BYTE_RUN = 15;
const uint16_t CHUNK_COPY = 16;
const uint16_t CHUNK_FRAME = 0xF1FA;

// DELTA_FLC line words: the top bits tell a packet count (00), a line skip
// (11) or the last pixel of an odd-width line (10).
const int32_t OPCODE_MASK = 0xC000;
const int32_t OPCODE_SKIP = 0xC000;
const int32_t OPCODE_LAST_BYTE = 0x8000;
const int32_t SKIP_BASE = 0x10000;
const int32_t LEVELS_64_SCALE = 4;
const int32_t FULL_COUNT = 256;

} // namespace

bool Flc::load(const uint8_t* bytes, int32_t byteCount, Flc* outFlc) {
    if (!outFlc) {
        return false;
    }

    if (byteCount < HEADER_SIZE || binU16(bytes, 4) != MAGIC) {
        return false;
    }

    Flc flc;
    flc.bytes_.assign(bytes, bytes + byteCount);
    flc.frameCount_ = (int32_t)binU16(bytes, 6);
    flc.width_ = (int32_t)binU16(bytes, 8);
    flc.height_ = (int32_t)binU16(bytes, 10);
    flc.speed_ = (int32_t)binU32(bytes, 16);
    const int32_t first = (int32_t)binU32(bytes, FIRST_FRAME_OFFSET);
    // A first frame offset of 0 means the header's end: the C#'s
    // `first == 0 ? HeaderSize : first`.
    flc.firstFrame_ = first == 0 ? HEADER_SIZE : first;
    flc.indices_.assign((size_t)flc.width_ * (size_t)flc.height_, 0);
    flc.palette_.assign(PALETTE_BYTES, 0);
    flc.rewind();

    *outFlc = flc;
    return true;
}

void Flc::rewind() {
    pos_ = firstFrame_;
    frame_ = 0;
    memset(indices_.data(), 0, indices_.size());
}

Flc::FrameStatus Flc::nextFrame() {
    if (frame_ >= frameCount_) {
        return FrameStatus::End;
    }

    const int32_t length = (int32_t)bytes_.size();
    while (pos_ + CHUNK_HEADER_SIZE <= length) {
        const int32_t size = (int32_t)binU32(bytes_.data(), pos_);
        const uint16_t type = binU16(bytes_.data(), pos_ + 4);
        const int32_t start = pos_;
        // A chunk smaller than its own header still advances by the header:
        // the C#'s Math.Max(size, ChunkHeaderSize).
        pos_ += size > CHUNK_HEADER_SIZE ? size : CHUNK_HEADER_SIZE;
        if (type != CHUNK_FRAME) {
            continue;
        }

        const int32_t chunks = (int32_t)binU16(bytes_.data(), start + 6);
        int32_t p = start + FRAME_HEADER_SIZE;
        for (int32_t i = 0; i < chunks; i++) {
            const int32_t chunkSize = (int32_t)binU32(bytes_.data(), p);
            if (!decodeChunk(binU16(bytes_.data(), p + 4), p + CHUNK_HEADER_SIZE)) {
                return FrameStatus::Broken;
            }

            p += chunkSize > CHUNK_HEADER_SIZE ? chunkSize : CHUNK_HEADER_SIZE;
        }

        frame_++;
        return FrameStatus::Decoded;
    }

    return FrameStatus::End;
}

bool Flc::decodeChunk(uint16_t type, int32_t at) {
    switch (type) {
    case CHUNK_COLOR_256:
        return decodePalette(at, 1);
    case CHUNK_COLOR_64:
        return decodePalette(at, LEVELS_64_SCALE);
    case CHUNK_BYTE_RUN:
        return decodeByteRun(at);
    case CHUNK_DELTA_FLC:
        return decodeDeltaFlc(at);
    case CHUNK_DELTA_FLI:
        return decodeDeltaFli(at);
    case CHUNK_BLACK:
        memset(indices_.data(), 0, indices_.size());
        return true;
    case CHUNK_COPY: {
        // The chunk holds a whole frame of raw indices.
        if (at + (int32_t)indices_.size() > (int32_t)bytes_.size()) {
            return false;
        }

        memcpy(indices_.data(), bytes_.data() + at, indices_.size());
        return true;
    }
    default:
        // An unknown chunk is skipped, as the C#'s switch does.
        return true;
    }
}

bool Flc::decodePalette(int32_t at, int32_t scale) {
    const int32_t length = (int32_t)bytes_.size();
    if (at + 2 > length) {
        return false;
    }

    const int32_t packets = (int32_t)binU16(bytes_.data(), at);
    int32_t p = at + 2;
    int32_t colour = 0;
    for (int32_t i = 0; i < packets; i++) {
        if (p + 2 > length) {
            return false;
        }

        colour += bytes_[(size_t)p];
        // A count of zero means 256 colours.
        int32_t count = bytes_[(size_t)p + 1];
        count = count == 0 ? FULL_COUNT : count;
        p += 2;
        if (p + count * 3 > length) {
            return false;
        }

        for (int32_t k = 0; k < count * 3; k++) {
            if (colour * 3 + k < PALETTE_BYTES) {
                const int32_t value = (int32_t)bytes_[(size_t)p + (size_t)k] * scale;
                palette_[(size_t)(colour * 3 + k)] = (uint8_t)(value > 255 ? 255 : value);
            }
        }

        p += count * 3;
        colour += count;
    }

    return true;
}

bool Flc::decodeByteRun(int32_t at) {
    const int32_t length = (int32_t)bytes_.size();
    int32_t p = at;
    for (int32_t y = 0; y < height_; y++) {
        // The packet count of the line, which the C# skips.
        p++;
        int32_t x = 0;
        const int32_t row = y * width_;
        while (x < width_) {
            if (p >= length) {
                return false;
            }

            const int32_t count = (int32_t)(int8_t)bytes_[(size_t)p++];
            if (count < 0) {
                const int32_t run = -count;
                if (p + run > length || row + x + run > (int32_t)indices_.size()) {
                    return false;
                }

                memcpy(indices_.data() + row + x, bytes_.data() + p, (size_t)run);
                p += run;
                x += run;
                continue;
            }

            if (p >= length || row + x + count > (int32_t)indices_.size()) {
                return false;
            }

            memset(indices_.data() + row + x, bytes_[(size_t)p], (size_t)count);
            p++;
            x += count;
        }
    }

    return true;
}

bool Flc::decodeDeltaFlc(int32_t at) {
    const int32_t length = (int32_t)bytes_.size();
    if (at + 2 > length) {
        return false;
    }

    const int32_t lines = (int32_t)binU16(bytes_.data(), at);
    int32_t p = at + 2;
    int32_t y = 0;
    for (int32_t line = 0; line < lines; line++) {
        int32_t packets = 0;
        while (true) {
            if (p + 2 > length) {
                return false;
            }

            const int32_t word = (int32_t)binU16(bytes_.data(), p);
            p += 2;
            if ((word & OPCODE_MASK) == OPCODE_SKIP) {
                y += SKIP_BASE - word;
            } else if ((word & OPCODE_MASK) == OPCODE_LAST_BYTE) {
                // The last pixel of an odd-width line. The line number is
                // counted in 64 bits: a skip can push it far past the
                // picture, and the check has to see that without wrapping.
                if (y < 0 || (int64_t)y * width_ + width_ - 1 >= (int64_t)indices_.size()) {
                    return false;
                }

                indices_[(size_t)((int64_t)y * width_ + width_ - 1)] = (uint8_t)word;
            } else {
                packets = word;
                break;
            }
        }

        int32_t x = 0;
        if (y < 0 || (int64_t)y * width_ >= (int64_t)indices_.size()) {
            return false;
        }

        const int64_t row = (int64_t)y * width_;
        for (int32_t i = 0; i < packets; i++) {
            if (p + 2 > length) {
                return false;
            }

            x += bytes_[(size_t)p];
            const int32_t count = (int32_t)(int8_t)bytes_[(size_t)p + 1];
            p += 2;
            if (count > 0) {
                const int32_t run = count * 2;
                if (p + run > length || row + x + run > (int64_t)indices_.size()) {
                    return false;
                }

                memcpy(indices_.data() + row + x, bytes_.data() + p, (size_t)run);
                p += run;
                x += run;
                continue;
            }

            if (p + 2 > length) {
                return false;
            }

            const uint8_t a = bytes_[(size_t)p];
            const uint8_t b = bytes_[(size_t)p + 1];
            p += 2;
            for (int32_t k = 0; k < -count; k++) {
                if (row + x + 1 >= (int64_t)indices_.size()) {
                    return false;
                }

                indices_[(size_t)(row + x)] = a;
                indices_[(size_t)(row + x + 1)] = b;
                x += 2;
            }
        }

        y++;
    }

    return true;
}

bool Flc::decodeDeltaFli(int32_t at) {
    const int32_t length = (int32_t)bytes_.size();
    if (at + 4 > length) {
        return false;
    }

    int32_t y = (int32_t)binU16(bytes_.data(), at);
    const int32_t lines = (int32_t)binU16(bytes_.data(), at + 2);
    int32_t p = at + 4;
    for (int32_t line = 0; line < lines; line++) {
        if (p >= length) {
            return false;
        }

        const int32_t packets = bytes_[(size_t)p++];
        int32_t x = 0;
        if (y < 0 || (int64_t)y * width_ >= (int64_t)indices_.size()) {
            return false;
        }

        const int64_t row = (int64_t)y * width_;
        for (int32_t i = 0; i < packets; i++) {
            if (p + 2 > length) {
                return false;
            }

            x += bytes_[(size_t)p];
            const int32_t count = (int32_t)(int8_t)bytes_[(size_t)p + 1];
            p += 2;
            if (count > 0) {
                if (p + count > length || row + x + count > (int64_t)indices_.size()) {
                    return false;
                }

                memcpy(indices_.data() + row + x, bytes_.data() + p, (size_t)count);
                p += count;
                x += count;
                continue;
            }

            if (p >= length || row + x - count > (int64_t)indices_.size()) {
                return false;
            }

            memset(indices_.data() + row + x, bytes_[(size_t)p], (size_t)(-count));
            p++;
            x += -count;
        }

        y++;
    }

    return true;
}

} // namespace formats
} // namespace mdk
