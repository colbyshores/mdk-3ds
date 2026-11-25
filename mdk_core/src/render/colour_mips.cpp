// colour_mips.cpp -- see colour_mips.h. The arithmetic is the C#'s own,
// texel for texel (cold path: texture creation, never per frame).

#include "render/colour_mips.h"

namespace mdk {
namespace {

const int32_t kChannels = 4;

int32_t halfOf(int32_t size) {
    return size / 2 > 1 ? size / 2 : 1;
}

int32_t levelSize(int32_t width, int32_t height) {
    return width * height * kChannels;
}

// The source texels [first, last) a texel of the next level covers: two,
// the whole size when it is 1, and the odd one left over joins the last
// (ColourMips.Span).
void spanOf(int32_t index, int32_t size, int32_t half, int32_t* first, int32_t* last) {
    *first = index * 2 < size - 1 ? index * 2 : size - 1;
    *last = index == half - 1 ? size : *first + 2;
}

// The next level: each texel the rounded mean of the block it covers.
void downsample(const uint8_t* rgba, int32_t width, int32_t height, uint8_t* result) {
    const int32_t halfWidth = halfOf(width);
    const int32_t halfHeight = halfOf(height);
    for (int32_t y = 0; y < halfHeight; y++) {
        int32_t top = 0;
        int32_t bottom = 0;
        spanOf(y, height, halfHeight, &top, &bottom);
        for (int32_t x = 0; x < halfWidth; x++) {
            int32_t left = 0;
            int32_t right = 0;
            spanOf(x, width, halfWidth, &left, &right);
            int32_t sum[kChannels] = {0, 0, 0, 0};
            for (int32_t sy = top; sy < bottom; sy++) {
                for (int32_t sx = left; sx < right; sx++) {
                    for (int32_t c = 0; c < kChannels; c++) {
                        sum[c] += rgba[((size_t)sy * (size_t)width + (size_t)sx) * kChannels + c];
                    }
                }
            }
            // The rounded mean over the block. The block is 1, 2 or 3
            // texels a side, so the count is one of 1, 2, 3, 4, 6, 9 --
            // spelt with literal divisors (the ARM11 has no integer
            // divide; a runtime divisor here would be a software helper
            // per texel of a texture build).
            const int32_t count = (bottom - top) * (right - left);
            for (int32_t c = 0; c < kChannels; c++) {
                int32_t mean = sum[c];
                switch (count) {
                    case 1: mean = sum[c]; break;
                    case 2: mean = (sum[c] + 1) / 2; break;
                    case 3: mean = (sum[c] + 1) / 3; break;
                    case 4: mean = (sum[c] + 2) / 4; break;
                    case 6: mean = (sum[c] + 3) / 6; break;
                    default: mean = (sum[c] + 4) / 9; break;
                }
                result[((size_t)y * (size_t)halfWidth + (size_t)x) * kChannels + c] =
                    (uint8_t)mean;
            }
        }
    }
}

} // namespace

int32_t colourLevels(int32_t width, int32_t height) {
    int32_t m = width > height ? width : height;
    if (m < 1) {
        m = 1;
    }
    int32_t levels = 1;
    while (m > 1) {
        m >>= 1;
        levels++;
    }
    return levels;
}

int32_t colourChainSize(int32_t width, int32_t height) {
    int32_t size = 0;
    const int32_t count = colourLevels(width, height);
    for (int32_t level = 0; level < count; level++) {
        size += levelSize(width, height);
        width = halfOf(width);
        height = halfOf(height);
    }
    return size;
}

void colourExpand(const uint8_t* indices, int32_t count, const uint8_t* palette,
                  int32_t paletteBytes, uint8_t* outRgba) {
    for (int32_t i = 0; i < count; i++) {
        uint8_t* texel = outRgba + (size_t)i * kChannels;
        const int32_t colour = indices[i] * kChannels;
        if (indices[i] == 0 || colour + kChannels > paletteBytes) {
            texel[0] = 0;
            texel[1] = 0;
            texel[2] = 0;
            texel[3] = 0;
            continue;
        }
        texel[0] = palette[colour];
        texel[1] = palette[colour + 1];
        texel[2] = palette[colour + 2];
        texel[3] = 255;
    }
}

void colourFillChain(uint8_t* chain, int32_t width, int32_t height) {
    int32_t offset = 0;
    const int32_t count = colourLevels(width, height);
    for (int32_t level = 1; level < count; level++) {
        const int32_t size = levelSize(width, height);
        downsample(chain + offset, width, height, chain + offset + size);
        offset += size;
        width = halfOf(width);
        height = halfOf(height);
    }
}

std::vector<uint8_t> colourChain(const uint8_t* indices, int32_t width, int32_t height,
                                 int32_t frames, const uint8_t* palette,
                                 int32_t paletteBytes) {
    const int32_t chainSize = colourChainSize(width, height);
    const int32_t frameSize = width * height;
    std::vector<uint8_t> out((size_t)chainSize * (size_t)frames);
    for (int32_t f = 0; f < frames; f++) {
        uint8_t* chain = out.data() + (size_t)f * (size_t)chainSize;
        colourExpand(indices + (size_t)f * (size_t)frameSize, frameSize, palette,
                     paletteBytes, chain);
        colourFillChain(chain, width, height);
    }
    return out;
}

} // namespace mdk
