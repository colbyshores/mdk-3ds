// ============================================================================
// gif.cpp — see gif.h for the structure and the LZW decoder's quirk.
// ============================================================================

#include "formats/gif.h"

#include <string.h>

#include "formats/bin_reader.h"

namespace mdk {
namespace formats {

namespace {

const int32_t HEADER_SIZE = 13;
const uint8_t IMAGE = 0x2C;
const uint8_t EXTENSION = 0x21;
// Packed fields: a colour table follows (2 << low 3 bits entries);
// interlaced rows.
const int32_t HAS_TABLE = 0x80;
const int32_t TABLE_SIZE = 0x07;
const int32_t INTERLACED = 0x40;
const int32_t MAX_CODE_SIZE = 12;
const int32_t PALETTE_BYTES = 768;

// Interlaced rows come in four passes: start row and step.
const int32_t PASS_START[4] = {0, 4, 2, 1};
const int32_t PASS_STEP[4] = {8, 8, 4, 2};

// The rows of an image, in the order they are stored.
void rowOrder(int32_t rows, bool interlaced, std::vector<int32_t>* out) {
    out->clear();
    if (!interlaced) {
        for (int32_t y = 0; y < rows; y++) {
            out->push_back(y);
        }

        return;
    }

    for (int32_t pass = 0; pass < 4; pass++) {
        for (int32_t y = PASS_START[pass]; y < rows; y += PASS_STEP[pass]) {
            out->push_back(y);
        }
    }
}

// Variable-width LZW: codes grow from min + 1 bits up to 12; a clear code
// starts over.
//
// Each code is a prefix code and its last byte; strings are rebuilt
// backwards. A code the table does not have yet is the previous string plus
// its own first byte.
bool lzw(const std::vector<uint8_t>& data, int32_t minSize, int32_t count, std::vector<uint8_t>* out) {
    const int32_t codes = 1 << MAX_CODE_SIZE;
    std::vector<uint8_t> output((size_t)count, 0);

    const int32_t clear = 1 << minSize;
    const int32_t end = clear + 1;

    std::vector<int32_t> prefixes((size_t)codes, 0);
    std::vector<uint8_t> suffixes((size_t)codes, 0);
    std::vector<uint8_t> firsts((size_t)codes, 0);
    for (int32_t i = 0; i < clear; i++) {
        prefixes[(size_t)i] = -1;
        suffixes[(size_t)i] = (uint8_t)i;
        firsts[(size_t)i] = (uint8_t)i;
    }

    std::vector<uint8_t> stack((size_t)codes, 0);
    int32_t size = minSize + 1;
    int32_t next = end + 1;
    int32_t previous = -1;
    uint32_t bits = 0;
    int32_t bitCount = 0;
    int32_t written = 0;
    int32_t p = 0;

    while (written < count) {
        while (bitCount < size && p < (int32_t)data.size()) {
            bits |= (uint32_t)data[(size_t)p++] << bitCount;
            bitCount += 8;
        }

        if (bitCount < size) {
            break;
        }

        const int32_t code = (int32_t)(bits & ((1u << size) - 1));
        bits >>= size;
        bitCount -= size;
        if (code == clear) {
            size = minSize + 1;
            next = end + 1;
            previous = -1;
            continue;
        }

        if (code == end) {
            break;
        }

        int32_t current = code;
        int32_t depth = 0;
        if (code >= next && previous >= 0) {
            stack[(size_t)depth++] = firsts[(size_t)previous];
            current = previous;
        }

        while (current >= 0) {
            if (depth >= codes) {
                return false;
            }

            stack[(size_t)depth++] = suffixes[(size_t)current];
            current = prefixes[(size_t)current];
        }

        for (int32_t k = depth - 1; k >= 0 && written < count; k--) {
            output[(size_t)written++] = stack[(size_t)k];
        }

        if (previous >= 0 && next < codes) {
            prefixes[(size_t)next] = previous;
            suffixes[(size_t)next] = stack[(size_t)depth - 1];
            firsts[(size_t)next] = firsts[(size_t)previous];
            next++;
            if (next == (1 << size) && size < MAX_CODE_SIZE) {
                size++;
            }
        }

        previous = code;
    }

    *out = output;
    return true;
}

// The colour table that follows a packed field byte.
bool readPalette(const uint8_t* bytes, int32_t byteCount, int32_t at, int32_t fields,
                 std::vector<uint8_t>* palette, int32_t* outAt) {
    const int32_t count = 2 << (fields & TABLE_SIZE);
    if (at < 0 || at + count * 3 > byteCount) {
        return false;
    }

    // The table is at the front of the palette; the rest stays black. The
    // count is never more than 256, so the copy always fits.
    memcpy(palette->data(), bytes + at, (size_t)count * 3);
    *outAt = at + count * 3;
    return true;
}

// The sub-blocks of an extension: each a length byte and that many bytes,
// ended by a zero length.
int32_t skipSubBlocks(const uint8_t* bytes, int32_t byteCount, int32_t at) {
    while (at < byteCount && bytes[at] != 0) {
        at += (int32_t)bytes[at] + 1;
    }

    return at + 1;
}

} // namespace

bool Gif::load(const uint8_t* bytes, int32_t byteCount, Gif* outGif) {
    if (!outGif) {
        return false;
    }

    if (byteCount < HEADER_SIZE) {
        return false;
    }

    char magic[4] = {0};
    binAscii(bytes, 0, 3, magic);
    if (strcmp(magic, "GIF") != 0) {
        return false;
    }

    Gif gif;
    gif.width_ = (int32_t)binU16(bytes, 6);
    gif.height_ = (int32_t)binU16(bytes, 8);
    gif.palette_.assign(PALETTE_BYTES, 0);
    gif.indices_.assign((size_t)gif.width_ * (size_t)gif.height_, 0);

    int32_t p = HEADER_SIZE;
    const int32_t fields = bytes[10];
    if ((fields & HAS_TABLE) != 0) {
        if (!readPalette(bytes, byteCount, p, fields, &gif.palette_, &p)) {
            return false;
        }
    }

    while (p < byteCount) {
        const uint8_t block = bytes[p++];
        if (block == EXTENSION) {
            p = skipSubBlocks(bytes, byteCount, p + 1);
            continue;
        }

        if (block != IMAGE) {
            return false;
        }

        // The image descriptor: left, top, width, height, packed fields.
        if (p + 9 > byteCount) {
            return false;
        }

        const int32_t left = (int32_t)binU16(bytes, p);
        const int32_t top = (int32_t)binU16(bytes, p + 2);
        const int32_t imageWidth = (int32_t)binU16(bytes, p + 4);
        const int32_t imageHeight = (int32_t)binU16(bytes, p + 6);
        const int32_t imageFields = bytes[p + 8];
        p += 9;
        if ((imageFields & HAS_TABLE) != 0) {
            if (!readPalette(bytes, byteCount, p, imageFields, &gif.palette_, &p)) {
                return false;
            }
        }

        if (p >= byteCount) {
            return false;
        }

        const int32_t codeSize = bytes[p++];

        // The LZW data, in sub-blocks.
        std::vector<uint8_t> data;
        while (p < byteCount && bytes[p] != 0) {
            const int32_t blockSize = bytes[p];
            if (p + 1 + blockSize > byteCount) {
                return false;
            }

            data.insert(data.end(), bytes + p + 1, bytes + p + 1 + blockSize);
            p += blockSize + 1;
        }

        std::vector<uint8_t> pixels;
        if (!lzw(data, codeSize, imageWidth * imageHeight, &pixels)) {
            return false;
        }

        // The picture goes into the screen's grid; a row past the bottom is
        // dropped and a row too wide is clipped.
        std::vector<int32_t> rows;
        rowOrder(imageHeight, (imageFields & INTERLACED) != 0, &rows);

        for (int32_t i = 0; i < imageHeight; i++) {
            const int32_t y = rows[(size_t)i] + top;
            if (y >= gif.height_) {
                continue;
            }

            const int32_t count = imageWidth < gif.width_ - left ? imageWidth : gif.width_ - left;
            if (count < 0) {
                return false;
            }

            const int32_t source = i * imageWidth;
            if (source + count > (int32_t)pixels.size()) {
                return false;
            }

            const int32_t target = y * gif.width_ + left;
            if (target + count > (int32_t)gif.indices_.size()) {
                return false;
            }

            memcpy(gif.indices_.data() + target, pixels.data() + source, (size_t)count);
        }

        *outGif = gif;
        return true;
    }

    // The file has no image descriptor at all.
    return false;
}

} // namespace formats
} // namespace mdk
