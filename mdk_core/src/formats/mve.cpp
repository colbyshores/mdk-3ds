// ============================================================================
// mve.cpp — see mve.h for the structure and the block codes.
//
// The block decoders below follow the C# exactly, including the places where
// it writes to the back buffer while reading from the frame shown last: that
// is how the original decodes, and the picture depends on it.
// ============================================================================

#include "formats/mve.h"

#include <string.h>
#include <utility>

#include "formats/bin_reader.h"

namespace mdk {
namespace formats {

namespace {

// The signature: "Interplay MVE File" and a 0x1a.
const char kSignature[] = "Interplay MVE File\x1a";
const int32_t kSignatureSize = 26;
const int32_t kChunkHeader = 4;
const int32_t kOpcodeHeader = 4;
const int32_t kPaletteBytes = 768;

// The opcodes.
enum Opcode : uint8_t {
    kEndOfStream = 0x00,
    kCreateTimer = 0x02,
    kInitAudio = 0x03,
    kInitVideo = 0x05,
    kSendBuffer = 0x07,
    kAudioFrame = 0x08,
    kSilenceFrame = 0x09,
    kSetPalette = 0x0C,
    kDecodingMap = 0x0F,
    kVideoData = 0x11,
};

// Audio flags: stereo, 16 bits, compressed (DPCM, from version 1).
const int32_t kAudioStereo = 1;
const int32_t kAudio16Bit = 2;
const int32_t kAudioCompressed = 4;
// The sound of stream 0 only.
const int32_t kStreamMask = 1;
// The video data starts with a 14-byte header.
const int32_t kVideoHeader = 14;
const int32_t kBlock = 8;
const float kSampleScale = 1.0f / 32768.0f;
const int32_t kByteMiddle = 128;
// The 8-bit pre-scale is a MULTIPLY and not a shift: `(byte - 128)` is negative
// for half the byte domain and C++17 makes a signed left shift UB there, so the
// shift form this used to carry was undefined for every quiet byte of every
// 8-bit cutscene track. Same reason, same spelled-out conversion as the audio
// decode (mdk_core/src/audio/sound.cpp); the two are deliberately not factored
// into one helper, because one constant does not justify a formats -> audio
// dependency, and a helper would hide the reason from the place that needs it.
const int32_t kByteScale = 256;

// Interplay DPCM: each byte indexes this table of steps.
const int16_t kDeltas[256] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 47, 51, 56, 61,
    66, 72, 79, 86, 94, 102, 112, 122, 133, 145, 158, 173, 189, 206, 225, 245,
    267, 292, 318, 348, 379, 414, 452, 493, 538, 587, 640, 699, 763, 832, 908, 991,
    1081, 1180, 1288, 1405, 1534, 1673, 1826, 1993, 2175, 2373, 2590, 2826, 3084, 3365, 3672, 4008,
    4373, 4772, 5208, 5683, 6202, 6767, 7385, 8059, 8794, 9597, 10472, 11428, 12471, 13609, 14851, 16206,
    17685, 19298, 21060, 22981, 25078, 27367, 29864, 32589, -29973, -26728, -23186, -19322, -15105, -10503, -5481, -1,
    1, 1, 5481, 10503, 15105, 19322, 23186, 26728, 29973, -32589, -29864, -27367, -25078, -22981, -21060, -19298,
    -17685, -16206, -14851, -13609, -12471, -11428, -10472, -9597, -8794, -8059, -7385, -6767, -6202, -5683, -5208, -4772,
    -4373, -4008, -3672, -3365, -3084, -2826, -2590, -2373, -2175, -1993, -1826, -1673, -1534, -1405, -1288, -1180,
    -1081, -991, -908, -832, -763, -699, -640, -587, -538, -493, -452, -414, -379, -348, -318, -292,
    -267, -245, -225, -206, -189, -173, -158, -145, -133, -122, -112, -102, -94, -86, -79, -72,
    -66, -61, -56, -51, -47, -43, -42, -41, -40, -39, -38, -37, -36, -35, -34, -33,
    -32, -31, -30, -29, -28, -27, -26, -25, -24, -23, -22, -21, -20, -19, -18, -17,
    -16, -15, -14, -13, -12, -11, -10, -9, -8, -7, -6, -5, -4, -3, -2, -1,
};

} // namespace

bool Mve::load(const uint8_t* bytes, int32_t byteCount, Mve* outMve) {
    if (!outMve) {
        return false;
    }

    if (byteCount < kSignatureSize) {
        return false;
    }

    char signature[26] = {0};
    binAscii(bytes, 0, (int32_t)strlen(kSignature), signature);
    if (memcmp(signature, kSignature, strlen(kSignature)) != 0) {
        return false;
    }

    Mve mve;
    mve.bytes_.assign(bytes, bytes + byteCount);
    mve.palette_.assign(kPaletteBytes, 0);
    mve.pos_ = kSignatureSize;

    // Moved, not copied: the file's bytes are the whole movie.
    *outMve = std::move(mve);
    return true;
}

Mve::FrameStatus Mve::nextFrame() {
    audio_.clear();
    shown_ = false;

    const int32_t length = (int32_t)bytes_.size();
    while (!ended_ && !shown_ && pos_ + kChunkHeader <= length) {
        const int32_t size = (int32_t)binU16(bytes_.data(), pos_);
        const int32_t end = pos_ + kChunkHeader + size;
        pos_ += kChunkHeader;
        while (pos_ + kOpcodeHeader <= end) {
            if (pos_ + 2 > length) {
                return FrameStatus::Broken;
            }

            const int32_t opcodeLength = (int32_t)binU16(bytes_.data(), pos_);
            const uint8_t type = bytes_[(size_t)pos_ + 2];
            const uint8_t version = bytes_[(size_t)pos_ + 3];
            if (!run(type, version, pos_ + kOpcodeHeader, opcodeLength)) {
                return FrameStatus::Broken;
            }

            pos_ += kOpcodeHeader + opcodeLength;
            ended_ = ended_ || type == kEndOfStream;
        }

        pos_ = end;
    }

    if (shown_) {
        frame_++;
    }

    return shown_ ? FrameStatus::Decoded : FrameStatus::End;
}

bool Mve::run(uint8_t type, uint8_t version, int32_t at, int32_t length) {
    const int32_t fileSize = (int32_t)bytes_.size();
    switch (type) {
    case kCreateTimer:
        if (at + 6 > fileSize) {
            return false;
        }

        frameTime_ = (int32_t)binU32(bytes_.data(), at) * (int32_t)binU16(bytes_.data(), at + 4);
        break;
    case kInitAudio:
        if (at + 6 > fileSize) {
            return false;
        }

        audioFlags_ = (int32_t)binU16(bytes_.data(), at + 2);
        // Version 0 has no compressed sound, whatever the flags say.
        if (version == 0) {
            audioFlags_ &= ~kAudioCompressed;
        }

        sampleRate_ = (int32_t)binU16(bytes_.data(), at + 4);
        channels_ = (audioFlags_ & kAudioStereo) != 0 ? 2 : 1;
        break;
    case kInitVideo:
        if (at + 4 > fileSize) {
            return false;
        }

        initVideo((int32_t)binU16(bytes_.data(), at) * kBlock,
                  (int32_t)binU16(bytes_.data(), at + 2) * kBlock);
        break;
    case kSetPalette:
        return setPalette(at);
    case kDecodingMap:
        if (at < 0 || length < 0 || at + length > fileSize) {
            return false;
        }

        map_.assign(bytes_.begin() + at, bytes_.begin() + at + length);
        break;
    case kVideoData:
        return decodeVideo(at + kVideoHeader, at + length);
    case kSendBuffer:
        // The back buffer becomes the frame shown last, and the old one
        // becomes the new back buffer.
        last_.swap(current_);
        shown_ = true;
        break;
    case kAudioFrame:
        if (at + 6 > fileSize) {
            return false;
        }

        if ((binU16(bytes_.data(), at + 2) & kStreamMask) != 0) {
            if (!decodeAudio(at + 6, at + length, (int32_t)binU16(bytes_.data(), at + 4))) {
                return false;
            }
        }

        break;
    case kSilenceFrame:
        if (at + 6 > fileSize) {
            return false;
        }

        if ((binU16(bytes_.data(), at + 2) & kStreamMask) != 0) {
            const int32_t count = (int32_t)binU16(bytes_.data(), at + 4) / (2 * channels_);
            for (int32_t i = 0; i < count; i++) {
                AudioSample sample;
                sample.left = 0.0f;
                sample.right = 0.0f;
                audio_.push_back(sample);
            }
        }

        break;
    default:
        // An unknown opcode is skipped, as the C#'s switch does.
        break;
    }

    return true;
}

void Mve::initVideo(int32_t width, int32_t height) {
    width_ = width;
    height_ = height;
    current_.assign((size_t)width * (size_t)height, 0);
    last_.assign((size_t)width * (size_t)height, 0);
}

bool Mve::setPalette(int32_t at) {
    const int32_t fileSize = (int32_t)bytes_.size();
    if (at + 4 > fileSize) {
        return false;
    }

    const int32_t start = (int32_t)binU16(bytes_.data(), at);
    const int32_t count = (int32_t)binU16(bytes_.data(), at + 2);
    int32_t p = at + 4;
    for (int32_t i = 0; i < count * 3; i++) {
        const int32_t index = start * 3 + i;
        if (index >= kPaletteBytes) {
            break;
        }

        if (p + i >= fileSize) {
            return false;
        }

        // 6-bit levels scaled to 8 bits as ffmpeg does (v * 4 + v / 16).
        const uint8_t v = bytes_[(size_t)(p + i)] & 0x3F;
        palette_[(size_t)index] = (uint8_t)((v << 2) | (v >> 4));
    }

    return true;
}

bool Mve::decodeAudio(int32_t at, int32_t end, int32_t size) {
    if ((audioFlags_ & kAudioCompressed) == 0) {
        return decodePcm(at, end);
    }

    const int32_t fileSize = (int32_t)bytes_.size();
    const int32_t count = size / 2;
    // The declared size must leave room for one PREDICTOR SLOT PER CHANNEL, which
    // the loop below fills before any loop condition can stop it: `written` is
    // bounded by `count` only inside the delta loop, so a size of 2*count with
    // count < channels_ wrote predictors_ straight past the end of `samples`.
    // Measured with -fsanitize=address on a stereo frame declaring size 2
    // (count 1, two channels): "heap-buffer-overflow ... WRITE of size 4" here.
    // The C# cannot do this — `new int[size / 2]` followed by
    // `samples[count++] = _predictors[ch]` raises IndexOutOfRange, and mve.h
    // documents that this port answers that throw with FrameStatus::Broken, so
    // returning false IS the oracle's behaviour, not a hardening invented here.
    // It also subsumes the old `count < 0` test: channels_ is 1 or 2, so a
    // negative count was already below this bound (and `size` is a u16, so there
    // was never a negative count to catch).
    if (count < channels_) {
        return false;
    }

    std::vector<int32_t> samples((size_t)count, 0);
    int32_t written = 0;
    int32_t p = at;
    for (int32_t ch = 0; ch < channels_; ch++) {
        if (p + 2 > fileSize) {
            return false;
        }

        predictors_[ch] = (int32_t)binS16(bytes_.data(), p);
        samples[(size_t)written++] = predictors_[ch];
        p += 2;
    }

    int32_t channel = 0;
    while (written < count && p < end) {
        if (p >= fileSize) {
            return false;
        }

        int32_t value = predictors_[channel] + (int32_t)kDeltas[bytes_[(size_t)p]];
        if (value < -32768) {
            value = -32768;
        } else if (value > 32767) {
            value = 32767;
        }

        predictors_[channel] = value;
        samples[(size_t)written++] = value;
        p++;
        channel ^= channels_ - 1;
    }

    pushSamples(samples.data(), written);
    return true;
}

bool Mve::decodePcm(int32_t at, int32_t end) {
    const int32_t fileSize = (int32_t)bytes_.size();
    const int32_t step = (audioFlags_ & kAudio16Bit) != 0 ? 2 : 1;
    std::vector<int32_t> samples;
    int32_t p = at;
    for (; p + step <= end; p += step) {
        if (p + step > fileSize) {
            return false;
        }

        samples.push_back(step == 2 ? (int32_t)binS16(bytes_.data(), p)
                                    : ((int32_t)bytes_[(size_t)p] - kByteMiddle) * kByteScale);
    }

    pushSamples(samples.data(), (int32_t)samples.size());
    return true;
}

void Mve::pushSamples(const int32_t* samples, int32_t count) {
    if (channels_ == 1) {
        for (int32_t i = 0; i < count; i++) {
            AudioSample sample;
            sample.left = (float)samples[i] * kSampleScale;
            sample.right = sample.left;
            audio_.push_back(sample);
        }

        return;
    }

    for (int32_t i = 0; i + 1 < count; i += 2) {
        AudioSample sample;
        sample.left = (float)samples[i] * kSampleScale;
        sample.right = (float)samples[i + 1] * kSampleScale;
        audio_.push_back(sample);
    }
}

bool Mve::decodeVideo(int32_t at, int32_t end) {
    int32_t block = 0;
    for (int32_t by = 0; by < height_; by += kBlock) {
        for (int32_t bx = 0; bx < width_; bx += kBlock) {
            const int32_t mapAt = block >> 1;
            if (mapAt >= (int32_t)map_.size()) {
                return false;
            }

            // Four bits per block, the low nibble first.
            const int32_t code = (block & 1) != 0 ? (map_[(size_t)mapAt] >> 4)
                                                 : (map_[(size_t)mapAt] & 0x0F);
            block++;
            int32_t next = at;
            if (!decodeBlock(code, bx, by, at, &next)) {
                return false;
            }

            at = next;
            if (at > end) {
                return true;
            }
        }
    }

    return true;
}

bool Mve::decodeBlock(int32_t code, int32_t x, int32_t y, int32_t at, int32_t* outAt) {
    const int32_t fileSize = (int32_t)bytes_.size();
    const int32_t o = y * width_ + x;

    switch (code) {
    case 0x0:
        // Unchanged: the block stays as it is.
        copy(o, 0, 0, true);
        *outAt = at;
        return true;
    case 0x1:
        // Skipped.
        *outAt = at;
        return true;
    case 0x2:
    case 0x3: {
        if (at >= fileSize) {
            return false;
        }

        const int32_t b = bytes_[(size_t)at];
        if (code == 0x2) {
            if (b < 56) {
                copy(o, 8 + b % 7, b / 7, false);
            } else {
                copy(o, -14 + (b - 56) % 29, 8 + (b - 56) / 29, false);
            }
        } else {
            if (b < 56) {
                copy(o, -(8 + b % 7), -(b / 7), false);
            } else {
                copy(o, -(-14 + (b - 56) % 29), -(8 + (b - 56) / 29), false);
            }
        }

        *outAt = at + 1;
        return true;
    }
    case 0x4: {
        if (at >= fileSize) {
            return false;
        }

        const int32_t b = bytes_[(size_t)at];
        copy(o, -8 + (b & 0x0F), -8 + (b >> 4), true);
        *outAt = at + 1;
        return true;
    }
    case 0x5: {
        if (at + 2 > fileSize) {
            return false;
        }

        copy(o, (int32_t)(int8_t)bytes_[(size_t)at], (int32_t)(int8_t)bytes_[(size_t)at + 1], true);
        *outAt = at + 2;
        return true;
    }
    case 0x7:
        return twoColours(o, at, outAt);
    case 0x8:
        return twoColourQuadrants(o, at, outAt);
    case 0x9:
        return fourColours(o, at, outAt);
    case 0xA:
        return fourColourQuadrants(o, at, outAt);
    case 0xB: {
        // Eight rows of eight raw indices.
        if (at + kBlock * kBlock > fileSize) {
            return false;
        }

        for (int32_t row = 0; row < kBlock; row++) {
            memcpy(current_.data() + o + row * width_, bytes_.data() + at + row * kBlock, kBlock);
        }

        *outAt = at + kBlock * kBlock;
        return true;
    }
    case 0xC: {
        // Sixteen 2x2 blocks of one colour each.
        if (at + 16 > fileSize) {
            return false;
        }

        for (int32_t i = 0; i < 16; i++) {
            fill(o + (i >> 2) * 2 * width_ + (i & 3) * 2, 2, 2, bytes_[(size_t)(at + i)]);
        }

        *outAt = at + 16;
        return true;
    }
    case 0xD: {
        // Four 4x4 blocks of one colour each.
        if (at + 4 > fileSize) {
            return false;
        }

        for (int32_t i = 0; i < 4; i++) {
            fill(o + (i >> 1) * 4 * width_ + (i & 1) * 4, 4, 4, bytes_[(size_t)(at + i)]);
        }

        *outAt = at + 4;
        return true;
    }
    case 0xE: {
        if (at >= fileSize) {
            return false;
        }

        fill(o, kBlock, kBlock, bytes_[(size_t)at]);
        *outAt = at + 1;
        return true;
    }
    case 0xF: {
        // A checkerboard of two colours.
        if (at + 2 > fileSize) {
            return false;
        }

        for (int32_t row = 0; row < kBlock; row++) {
            for (int32_t col = 0; col < kBlock; col++) {
                current_[(size_t)(o + row * width_ + col)] = bytes_[(size_t)(at + ((row + col) & 1))];
            }
        }

        *outAt = at + 2;
        return true;
    }
    default:
        *outAt = at;
        return true;
    }
}

void Mve::copy(int32_t at, int32_t dx, int32_t dy, bool fromLast) {
    const std::vector<uint8_t>& source = fromLast ? last_ : current_;
    const int64_t from = (int64_t)at + (int64_t)dy * width_ + dx;
    if (from < 0 || from + 7 * (int64_t)width_ + kBlock > (int64_t)source.size()) {
        return;
    }

    for (int32_t row = 0; row < kBlock; row++) {
        // Overlapping copies within the back buffer go pixel by pixel, as the
        // original does.
        for (int32_t col = 0; col < kBlock; col++) {
            current_[(size_t)(at + row * width_ + col)] = source[(size_t)(from + row * width_ + col)];
        }
    }
}

void Mve::fill(int32_t at, int32_t width, int32_t height, uint8_t value) {
    for (int32_t row = 0; row < height; row++) {
        const int64_t start = (int64_t)at + (int64_t)row * width_;
        if (start < 0 || start + width > (int64_t)current_.size()) {
            return;
        }

        memset(current_.data() + start, value, (size_t)width);
    }
}

bool Mve::twoColours(int32_t at, int32_t p, int32_t* outAt) {
    const int32_t fileSize = (int32_t)bytes_.size();
    if (p + 2 > fileSize) {
        return false;
    }

    const uint8_t p0 = bytes_[(size_t)p];
    const uint8_t p1 = bytes_[(size_t)p + 1];
    p += 2;
    if (p0 <= p1) {
        // A bit per pixel, a row each, the low bit first.
        if (p + kBlock > fileSize) {
            return false;
        }

        for (int32_t row = 0; row < kBlock; row++) {
            const int32_t flags = bytes_[(size_t)(p + row)];
            for (int32_t col = 0; col < kBlock; col++) {
                current_[(size_t)(at + row * width_ + col)] =
                    ((flags >> col) & 1) != 0 ? p1 : p0;
            }
        }

        *outAt = p + kBlock;
        return true;
    }

    // A bit per 2x2 block, 16 bits.
    if (p + 2 > fileSize) {
        return false;
    }

    const int32_t bits = (int32_t)binU16(bytes_.data(), p);
    for (int32_t i = 0; i < 16; i++) {
        fill(at + (i >> 2) * 2 * width_ + (i & 3) * 2, 2, 2, ((bits >> i) & 1) != 0 ? p1 : p0);
    }

    *outAt = p + 2;
    return true;
}

bool Mve::twoColourQuadrants(int32_t at, int32_t p, int32_t* outAt) {
    const int32_t fileSize = (int32_t)bytes_.size();
    if (p + 2 > fileSize) {
        return false;
    }

    uint8_t colours[2] = {bytes_[(size_t)p], bytes_[(size_t)p + 1]};
    if (colours[0] <= colours[1]) {
        // Two colours per 4x4 quadrant: top-left, bottom-left, top-right,
        // bottom-right; each with its colours and 16 bits.
        for (int32_t q = 0; q < 4; q++) {
            if (p + 4 > fileSize) {
                return false;
            }

            colours[0] = bytes_[(size_t)p];
            colours[1] = bytes_[(size_t)p + 1];
            const int32_t bits16 = (int32_t)binU16(bytes_.data(), p + 2);
            p += 4;
            const int32_t qo = at + (q & 1) * 4 * width_ + (q >> 1) * 4;
            for (int32_t i = 0; i < 16; i++) {
                current_[(size_t)(qo + (i >> 2) * width_ + (i & 3))] = colours[(bits16 >> i) & 1];
            }
        }

        *outAt = p;
        return true;
    }

    // Two colours per half: left and right, else top and bottom; 32 bits
    // each.
    if (p + 8 > fileSize) {
        return false;
    }

    uint32_t bits = binU32(bytes_.data(), p + 2);
    uint8_t other[2] = {bytes_[(size_t)p + 6], bytes_[(size_t)p + 7]};
    p += 8;
    const bool sideBySide = other[0] <= other[1];
    for (int32_t half = 0; half < 2; half++) {
        if (half == 1) {
            colours[0] = other[0];
            colours[1] = other[1];
            if (p + 4 > fileSize) {
                return false;
            }

            bits = binU32(bytes_.data(), p);
            p += 4;
        }

        for (int32_t i = 0; i < 32; i++) {
            const uint8_t c = colours[(bits >> i) & 1];
            const int32_t target = sideBySide ? at + (i >> 2) * width_ + half * 4 + (i & 3)
                                              : at + (half * 4 + (i >> 3)) * width_ + (i & 7);
            current_[(size_t)target] = c;
        }
    }

    *outAt = p;
    return true;
}

bool Mve::fourColours(int32_t at, int32_t p, int32_t* outAt) {
    const int32_t fileSize = (int32_t)bytes_.size();
    if (p + 4 > fileSize) {
        return false;
    }

    uint8_t colours[4] = {bytes_[(size_t)p], bytes_[(size_t)p + 1], bytes_[(size_t)p + 2],
                           bytes_[(size_t)p + 3]};
    p += 4;
    if (colours[0] <= colours[1]) {
        if (colours[2] <= colours[3]) {
            // Two bits per pixel, 16 bytes.
            if (p + 16 > fileSize) {
                return false;
            }

            for (int32_t row = 0; row < kBlock; row++) {
                const int32_t flags = (int32_t)binU16(bytes_.data(), p + row * 2);
                for (int32_t col = 0; col < kBlock; col++) {
                    current_[(size_t)(at + row * width_ + col)] = colours[(flags >> (col * 2)) & 3];
                }
            }

            *outAt = p + 16;
            return true;
        }

        // Two bits per 2x2 block, 4 bytes.
        if (p + 4 > fileSize) {
            return false;
        }

        const uint32_t bits = binU32(bytes_.data(), p);
        for (int32_t i = 0; i < 16; i++) {
            fill(at + (i >> 2) * 2 * width_ + (i & 3) * 2, 2, 2, colours[(bits >> (i * 2)) & 3]);
        }

        *outAt = p + 4;
        return true;
    }

    // 64 bits, over 2x1 or 1x2 pairs.
    if (p + 8 > fileSize) {
        return false;
    }

    const uint64_t all = (uint64_t)binU32(bytes_.data(), p) | ((uint64_t)binU32(bytes_.data(), p + 4) << 32);
    for (int32_t i = 0; i < 32; i++) {
        const uint8_t c = colours[(all >> (i * 2)) & 3];
        if (colours[2] <= colours[3]) {
            fill(at + (i >> 2) * width_ + (i & 3) * 2, 2, 1, c);
        } else {
            fill(at + (i >> 3) * 2 * width_ + (i & 7), 1, 2, c);
        }
    }

    *outAt = p + 8;
    return true;
}

bool Mve::fourColourQuadrants(int32_t at, int32_t p, int32_t* outAt) {
    const int32_t fileSize = (int32_t)bytes_.size();
    if (p + 4 > fileSize) {
        return false;
    }

    uint8_t colours[4] = {bytes_[(size_t)p], bytes_[(size_t)p + 1], bytes_[(size_t)p + 2],
                           bytes_[(size_t)p + 3]};
    if (colours[0] <= colours[1]) {
        // Four colours per 4x4 quadrant, each with its colours and 32 bits.
        for (int32_t q = 0; q < 4; q++) {
            if (p + 8 > fileSize) {
                return false;
            }

            colours[0] = bytes_[(size_t)p];
            colours[1] = bytes_[(size_t)p + 1];
            colours[2] = bytes_[(size_t)p + 2];
            colours[3] = bytes_[(size_t)p + 3];
            const uint32_t bits = binU32(bytes_.data(), p + 4);
            p += 8;
            const int32_t qo = at + (q & 1) * 4 * width_ + (q >> 1) * 4;
            for (int32_t i = 0; i < 16; i++) {
                current_[(size_t)(qo + (i >> 2) * width_ + (i & 3))] = colours[(bits >> (i * 2)) & 3];
            }
        }

        *outAt = p;
        return true;
    }

    // Four colours per half: left and right, else top and bottom; 64 bits
    // each.
    if (p + 16 > fileSize) {
        return false;
    }

    uint64_t all = (uint64_t)binU32(bytes_.data(), p + 4) | ((uint64_t)binU32(bytes_.data(), p + 8) << 32);
    uint8_t other[4] = {bytes_[(size_t)p + 12], bytes_[(size_t)p + 13], bytes_[(size_t)p + 14],
                         bytes_[(size_t)p + 15]};
    p += 16;
    const bool sideBySide = other[0] <= other[1];
    for (int32_t half = 0; half < 2; half++) {
        if (half == 1) {
            colours[0] = other[0];
            colours[1] = other[1];
            colours[2] = other[2];
            colours[3] = other[3];
            if (p + 8 > fileSize) {
                return false;
            }

            all = (uint64_t)binU32(bytes_.data(), p) | ((uint64_t)binU32(bytes_.data(), p + 4) << 32);
            p += 8;
        }

        for (int32_t i = 0; i < 32; i++) {
            const uint8_t c = colours[(all >> (i * 2)) & 3];
            const int32_t target = sideBySide ? at + (i >> 2) * width_ + half * 4 + (i & 3)
                                              : at + (half * 4 + (i >> 3)) * width_ + (i & 7);
            current_[(size_t)target] = c;
        }
    }

    *outAt = p;
    return true;
}

} // namespace formats
} // namespace mdk
