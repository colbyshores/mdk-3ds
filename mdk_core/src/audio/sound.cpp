// ============================================================================
// sound.cpp — the PCM decode (port of Sound.FromPcm).
// ============================================================================

#include "audio/sound.h"

namespace mdk {
namespace audio {

namespace {

// The C#'s ByteScale / ShortScale / ByteMiddle, as constants rather than
// inline literals so there is one of each and the rounding is named.
const float kByteScale = 1.0f / 128.0f;
const float kShortScale = 1.0f / 32768.0f;
const int32_t kByteMiddle = 128;

} // namespace

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

    if (bitsPerSample == 8) {
        out->samples.resize((size_t)byteCount);
        for (int32_t i = 0; i < byteCount; i++) {
            // (data[i] - 128) * ByteScale: the byte is unsigned, so the bias
            // is an int subtract and only the multiply is float, as in the C#.
            out->samples[(size_t)i] =
                (float)((int32_t)data[(size_t)i] - kByteMiddle) * kByteScale;
        }
        return;
    }

    // 16-bit: the even part of the chunk, one short per two bytes.
    const int32_t usable = byteCount & ~1;
    const int32_t count = usable / 2;
    out->samples.resize((size_t)count);
    for (int32_t i = 0; i < count; i++) {
        const int32_t at = i * 2;
        const int32_t raw = (int32_t)data[(size_t)at] | ((int32_t)data[(size_t)at + 1] << 8);
        // Sign-extend the 16-bit word the way a signed short would, then the
        // C#'s `shorts[i] * ShortScale`.
        out->samples[(size_t)i] = (float)(int16_t)(raw & 0xFFFF) * kShortScale;
    }
}

} // namespace audio
} // namespace mdk
