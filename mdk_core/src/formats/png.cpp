// ============================================================================
// png.cpp — see png.h for what is read and written.
//
// The zlib calls are the whole of the difference from the C#: the C#'s
// ZLibStream is zlib underneath, and its ReadExactly is an inflate that has
// to produce exactly the number of bytes the picture needs.
// ============================================================================

#include "formats/png.h"

#include <stdio.h>
#include <string.h>

#include <zlib.h>

namespace mdk {
namespace formats {

namespace {

const uint8_t kSignature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
const int32_t kHeaderSize = 13;
const int32_t kDepth = 8;
const uint8_t kOpaque = 255;
const int32_t kRgba = 4;

// Colour types (IHDR byte 9) and their bytes per pixel.
const uint8_t kGrey = 0;
const uint8_t kRgb = 2;
const uint8_t kGreyAlpha = 4;
const uint8_t kRgbAlpha = 6;

// Row filters: none, left, up, mean of left and up, Paeth.
const uint8_t kFilterNone = 0;
const uint8_t kFilterSub = 1;
const uint8_t kFilterUp = 2;
const uint8_t kFilterAverage = 3;
const uint8_t kFilterPaeth = 4;

void setError(std::string* outError, const char* message) {
    if (outError) {
        *outError = message;
    }
}

// The CRC-32 (ISO 3309) table, built once.
uint32_t crc32Update(uint32_t crc, const uint8_t* bytes, int32_t count) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        const uint32_t polynomial = 0xEDB88320;
        for (uint32_t n = 0; n < 256; n++) {
            uint32_t c = n;
            for (int32_t k = 0; k < 8; k++) {
                c = (c & 1) != 0 ? polynomial ^ (c >> 1) : c >> 1;
            }

            table[n] = c;
        }

        ready = true;
    }

    for (int32_t i = 0; i < count; i++) {
        crc = table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
    }

    return crc;
}

// A big-endian u32.
uint32_t readBigEndian32(const uint8_t* bytes) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

// A big-endian i32, as the C#'s BinaryPrimitives.ReadInt32BigEndian.
int32_t readBigEndianSigned32(const uint8_t* bytes) {
    return (int32_t)readBigEndian32(bytes);
}

void writeBigEndian32(std::vector<uint8_t>* out, uint32_t value) {
    out->push_back((uint8_t)(value >> 24));
    out->push_back((uint8_t)(value >> 16));
    out->push_back((uint8_t)(value >> 8));
    out->push_back((uint8_t)value);
}

// A chunk: u32 length, type, data, CRC-32 (big-endian).
void writeChunk(std::vector<uint8_t>* out, const char* type, const uint8_t* data, int32_t count) {
    writeBigEndian32(out, (uint32_t)count);
    const int32_t typeLength = 4;
    const uint8_t* typeBytes = (const uint8_t*)type;
    out->insert(out->end(), typeBytes, typeBytes + typeLength);
    if (count > 0) {
        out->insert(out->end(), data, data + count);
    }

    // The CRC covers the type and the data together.
    uint32_t crc = crc32Update(0xFFFFFFFFu, typeBytes, typeLength);
    if (count > 0) {
        crc = crc32Update(crc, data, count);
    }

    writeBigEndian32(out, crc ^ 0xFFFFFFFFu);
}

// The zlib stream of `source`, as the C#'s ZLibStream would write it.
bool deflate(const std::vector<uint8_t>& source, std::vector<uint8_t>* out) {
    uLongf size = compressBound((uLong)source.size());
    std::vector<uint8_t> buffer((size_t)size);
    // CompressionLevel.Optimal is zlib's default level, as it is in .NET.
    if (compress2(buffer.data(), &size, source.data(), (uLong)source.size(), Z_DEFAULT_COMPRESSION) != Z_OK) {
        return false;
    }

    buffer.resize((size_t)size);
    *out = buffer;
    return true;
}

// The zlib stream of `source`, inflated.
bool inflate(const uint8_t* data, int32_t count, std::vector<uint8_t>* out) {
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    if (inflateInit(&stream) != Z_OK) {
        return false;
    }

    stream.next_in = (Bytef*)data;
    stream.avail_in = (uInt)count;

    std::vector<uint8_t> result;
    uint8_t chunk[16384];
    int32_t status = Z_OK;
    do {
        stream.next_out = chunk;
        stream.avail_out = (uInt)sizeof(chunk);
        status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) {
            break;
        }

        result.insert(result.end(), chunk, chunk + (sizeof(chunk) - stream.avail_out));
    } while (status != Z_STREAM_END && (stream.avail_in > 0 || stream.avail_out == 0));

    inflateEnd(&stream);
    if (status != Z_STREAM_END) {
        return false;
    }

    *out = result;
    return true;
}

// The Paeth predictor.
int32_t paeth(int32_t a, int32_t b, int32_t c) {
    const int32_t p = a + b - c;
    const int32_t pa = p > a ? p - a : a - p;
    const int32_t pb = p > b ? p - b : b - p;
    const int32_t pc = p > c ? p - c : c - p;
    return pa <= pb && pa <= pc ? a : (pb <= pc ? b : c);
}

} // namespace

bool Png::toRgba(const std::vector<uint8_t>& raw, int32_t width, int32_t height, int32_t bpp,
                 std::vector<uint8_t>* outRgba) {
    std::vector<uint8_t> rgba((size_t)width * (size_t)height * kRgba, 0);
    for (int32_t y = 0; y < height; y++) {
        const int32_t row = y * (width * bpp + 1) + 1;
        for (int32_t x = 0; x < width; x++) {
            const int32_t source = row + x * bpp;
            if (source + bpp > (int32_t)raw.size()) {
                return false;
            }

            const int32_t target = (y * width + x) * kRgba;
            const bool colour = bpp >= 3;
            rgba[(size_t)target] = raw[(size_t)source];
            rgba[(size_t)target + 1] = colour ? raw[(size_t)source + 1] : raw[(size_t)source];
            rgba[(size_t)target + 2] = colour ? raw[(size_t)source + 2] : raw[(size_t)source];
            rgba[(size_t)target + 3] = (bpp == 2 || bpp == kRgba) ? raw[(size_t)(source + bpp - 1)] : kOpaque;
        }
    }

    *outRgba = rgba;
    return true;
}

bool Png::decode(const uint8_t* bytes, int32_t byteCount, Image* outImage, std::string* outError) {
    if (!outImage) {
        return false;
    }

    if (byteCount < (int32_t)sizeof(kSignature) || memcmp(bytes, kSignature, sizeof(kSignature)) != 0) {
        setError(outError, "Not a PNG");
        return false;
    }

    int32_t width = 0;
    int32_t height = 0;
    uint8_t type = 0;
    std::vector<uint8_t> compressed;

    int32_t at = (int32_t)sizeof(kSignature);
    while (at + 8 <= byteCount) {
        const int32_t length = readBigEndianSigned32(bytes + at);
        const char* name = (const char*)(bytes + at + 4);
        if (length < 0 || (int64_t)at + 12 + length > byteCount) {
            setError(outError, "PNG chunk past the end");
            return false;
        }

        const uint8_t* chunk = bytes + at + 8;
        at += 12 + length;
        if (memcmp(name, "IHDR", 4) == 0) {
            // The header: the width and the height as big-endian i32, the
            // depth, the colour type and the interlace flag.
            if (length < kHeaderSize || chunk[8] != kDepth || chunk[12] != 0) {
                setError(outError, "PNG not of 8-bit channels, or interlaced");
                return false;
            }

            width = readBigEndianSigned32(chunk);
            height = readBigEndianSigned32(chunk + 4);
            type = chunk[9];
        } else if (memcmp(name, "IDAT", 4) == 0) {
            compressed.insert(compressed.end(), chunk, chunk + length);
        } else if (memcmp(name, "IEND", 4) == 0) {
            break;
        }
    }

    if (width <= 0 || height <= 0) {
        setError(outError, "PNG without a header");
        return false;
    }
    // The widest and tallest read (upstream 09f2e511): 256 MB as RGBA8, far past
    // any mod's HD image; larger sizes would overflow the row arithmetic below
    // (width * bpp, (stride + 1) * height in int32) or exhaust memory. A mod is a
    // downloaded file.
    const int32_t kMaxSide = 8192;
    if (width > kMaxSide || height > kMaxSide) {
        setError(outError, "PNG over 8192 pixels a side");
        return false;
    }

    int32_t bpp = 0;
    switch (type) {
    case kGrey:
        bpp = 1;
        break;
    case kGreyAlpha:
        bpp = 2;
        break;
    case kRgb:
        bpp = 3;
        break;
    case kRgbAlpha:
        bpp = kRgba;
        break;
    default: {
        char message[64];
        snprintf(message, sizeof(message), "PNG colour type %u not read", (unsigned)type);
        setError(outError, message);
        return false;
    }
    }

    std::vector<uint8_t> raw;
    if (!inflate(compressed.data(), (int32_t)compressed.size(), &raw)) {
        setError(outError, "PNG data does not inflate");
        return false;
    }

    const int32_t stride = width * bpp;
    const int32_t needed = (stride + 1) * height;
    if ((int32_t)raw.size() < needed) {
        setError(outError, "PNG data is short");
        return false;
    }

    // Undo each row's filter in place; the filter byte stays before the row.
    for (int32_t y = 0; y < height; y++) {
        const int32_t start = y * (stride + 1) + 1;
        const uint8_t filter = raw[(size_t)(start - 1)];
        const int32_t previous = start - (stride + 1);
        for (int32_t x = 0; x < stride; x++) {
            const int32_t a = x >= bpp ? raw[(size_t)(start + x - bpp)] : 0;
            const int32_t b = y > 0 ? raw[(size_t)(previous + x)] : 0;
            const int32_t c = (x >= bpp && y > 0) ? raw[(size_t)(previous + x - bpp)] : 0;
            int32_t value = 0;
            switch (filter) {
            case kFilterNone:
                value = 0;
                break;
            case kFilterSub:
                value = a;
                break;
            case kFilterUp:
                value = b;
                break;
            case kFilterAverage:
                value = (a + b) / 2;
                break;
            case kFilterPaeth:
                value = paeth(a, b, c);
                break;
            default: {
                char message[64];
                snprintf(message, sizeof(message), "PNG row filter %u", (unsigned)filter);
                setError(outError, message);
                return false;
            }
            }

            raw[(size_t)(start + x)] = (uint8_t)(raw[(size_t)(start + x)] + (uint8_t)value);
        }
    }

    Image image;
    image.width = width;
    image.height = height;
    if (!toRgba(raw, width, height, bpp, &image.rgba)) {
        setError(outError, "PNG data is short");
        return false;
    }

    *outImage = image;
    return true;
}

bool Png::encode(int32_t width, int32_t height, const uint8_t* rgba, int32_t byteCount,
                 Channels channels, std::vector<uint8_t>* outBytes, std::string* outError) {
    if (!outBytes || width <= 0 || height <= 0) {
        setError(outError, "PNG needs a picture");
        return false;
    }

    const int32_t bpp = channels == Channels::Rgb ? 3 : kRgba;
    const int32_t stride = width * bpp;
    if ((int64_t)width * height * kRgba > byteCount) {
        setError(outError, "PNG needs the whole picture");
        return false;
    }

    // The rows, each with its filter byte in front.
    std::vector<uint8_t> raw((size_t)(stride + 1) * (size_t)height, 0);
    for (int32_t y = 0; y < height; y++) {
        uint8_t* row = raw.data() + (size_t)y * (stride + 1) + 1;
        for (int32_t x = 0; x < width; x++) {
            memcpy(row + (size_t)x * bpp, rgba + ((size_t)y * width + x) * kRgba, (size_t)bpp);
        }
    }

    std::vector<uint8_t> compressed;
    if (!deflate(raw, &compressed)) {
        setError(outError, "PNG data does not compress");
        return false;
    }

    std::vector<uint8_t> file;
    file.insert(file.end(), kSignature, kSignature + sizeof(kSignature));

    uint8_t header[kHeaderSize] = {0};
    header[0] = (uint8_t)((uint32_t)width >> 24);
    header[1] = (uint8_t)((uint32_t)width >> 16);
    header[2] = (uint8_t)((uint32_t)width >> 8);
    header[3] = (uint8_t)width;
    header[4] = (uint8_t)((uint32_t)height >> 24);
    header[5] = (uint8_t)((uint32_t)height >> 16);
    header[6] = (uint8_t)((uint32_t)height >> 8);
    header[7] = (uint8_t)height;
    header[8] = kDepth;
    header[9] = channels == Channels::Rgb ? kRgb : kRgbAlpha;
    writeChunk(&file, "IHDR", header, kHeaderSize);
    writeChunk(&file, "IDAT", compressed.data(), (int32_t)compressed.size());
    writeChunk(&file, "IEND", nullptr, 0);

    *outBytes = file;
    return true;
}

} // namespace formats
} // namespace mdk
