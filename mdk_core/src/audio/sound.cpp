// ============================================================================
// sound.cpp — the PCM decode (port of Sound.FromPcm).
//
// The decode writes the file's int16 domain, not the oracle's float, and does
// not touch a float at all: the bias is an int subtract, the 8-bit pre-scale is
// an int multiply (and not a shift — see kByteScale), and the 16-bit path is a
// byte pair. The scale that makes the
// oracle's value is applied at the read, in Sound::sampleAt(), once per tree.
// See sound.h for why the two are the same float bit for bit.
// ============================================================================

#include "audio/sound.h"

namespace mdk {
namespace audio {

namespace {

// The C#'s ByteMiddle, and the scale that puts a byte into the same int16
// domain as a short. ByteScale itself is gone: `(byte-128) * (1/128)` and
// `((byte-128) * 256) * (1/32768)` are the same dyadic value, so one scale now
// serves both widths (sound.h).
//
// IT IS A MULTIPLY AND NOT A SHIFT, AND THAT IS A DEFINITION ARGUMENT AND NOT A
// STYLE ONE. `(data[i] - 128)` is negative for every byte below 128, and
// left-shifting a negative signed value is undefined in C++17 ([expr.shift]/2:
// "if E1 has a signed type and a negative value, that behavior is undefined").
// Both builds are C++17 (`mdk_3ds/Makefile`, `tests/scripts/Makefile`), so the
// shift form was UB on half of the byte domain — every quiet half of every
// 8-bit sound in the game — and UBSan says so:
//
//     g++ -std=c++17 -O1 -fsanitize=undefined  ->  runtime error: left shift of
//     negative value -128
//
// The multiply is fully defined: the product spans [-32768, 32512], inside
// int32 (no overflow) and inside int16 (so the narrowing cast is in range and
// not implementation-defined either), and GCC lowers `* 256` to the same `lsl
// #8` the shift spelled. Same instruction, no undefined behaviour. The same
// idiom is spelled out the same way in formats/mve.cpp's 8-bit decode.
const int32_t kByteMiddle = 128;
const int32_t kByteScale = 256;

} // namespace

int32_t pcmSampleCount(int32_t byteCount, int32_t bitsPerSample) {
    if (byteCount <= 0) {
        return 0;
    }
    // 8-bit: one byte per sample; otherwise the even part of the chunk, one
    // short per two bytes.
    return bitsPerSample == 8 ? byteCount : (byteCount & ~1) / 2;
}

void pcmToSamples(const uint8_t* data, int32_t bitsPerSample, int32_t first, int32_t count,
                  int16_t* out) {
    if (bitsPerSample == 8) {
        for (int32_t i = 0; i < count; i++) {
            // (data[i] - 128) * 256: the byte is unsigned, so the bias is an int
            // subtract and the result spans [-32768, 32512] — inside int16 for
            // every byte, which is what lets the read share one scale with the
            // 16-bit path. No float appears in this loop, and the scale is a
            // multiply for the reason at its definition above.
            out[i] = (int16_t)(((int32_t)data[(size_t)(first + i)] - kByteMiddle) * kByteScale);
        }
        return;
    }
    for (int32_t i = 0; i < count; i++) {
        const int32_t at = (first + i) * 2;
        const int32_t raw = (int32_t)data[(size_t)at] | ((int32_t)data[(size_t)at + 1] << 8);
        // Sign-extend the 16-bit word the way a signed short would, and store it
        // as that short. The C#'s `shorts[i] * ShortScale` happens at the read.
        out[i] = (int16_t)(raw & 0xFFFF);
    }
}

void soundFromPcm(const uint8_t* data, int32_t byteCount, int32_t channels, int32_t sampleRate,
                  int32_t bitsPerSample, bool looping, Sound* out) {
    if (!out) {
        return;
    }
    out->channels = channels;
    out->sampleRate = sampleRate;
    out->looping = looping;
    out->samples.clear();
    if (!data || byteCount <= 0) {
        return;
    }
    const int32_t count = pcmSampleCount(byteCount, bitsPerSample);
    out->samples.resize((size_t)count);
    pcmToSamples(data, bitsPerSample, 0, count, out->samples.data());
}

} // namespace audio
} // namespace mdk
