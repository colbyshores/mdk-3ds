// ============================================================================
// dsp_adpcm.cpp -- see dsp_adpcm.h.
// ============================================================================

#include "audio/dsp_adpcm.h"

namespace mdk {
namespace audio {

namespace {

uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

int16_t be16(const uint8_t* p) { return (int16_t)(((uint16_t)p[0] << 8) | p[1]); }

// One frame: 14 samples from the header byte's predictor pair and scale.
void frame(const uint8_t* f, const int16_t* coefs, int32_t* h1, int32_t* h2, int16_t* out) {
    const int32_t scale = 1 << (f[0] & 0x0F);
    const int32_t pair = (f[0] >> 4) & 0x07;
    const int32_t c1 = coefs[pair * 2];
    const int32_t c2 = coefs[pair * 2 + 1];
    for (int32_t n = 0; n < DspAdpcm::kFrameSamples; n++) {
        const int32_t byte = f[1 + (n >> 1)];
        int32_t nibble = (n & 1) ? (byte & 0x0F) : (byte >> 4);
        if (nibble >= 8) {
            nibble -= 16;
        }
        int32_t v = ((nibble * scale) << 11) + c1 * *h1 + c2 * *h2;
        v = (v + 1024) >> 11;
        if (v > 32767) {
            v = 32767;
        } else if (v < -32768) {
            v = -32768;
        }
        *h2 = *h1;
        *h1 = v;
        if (out) {
            out[n] = (int16_t)v;
        }
    }
}

} // namespace

bool DspAdpcm::parseHeader(const uint8_t* bytes, int32_t byteCount, DspAdpcmHeader* out) {
    if (byteCount < kHeaderBytes) {
        return false;
    }
    DspAdpcmHeader h;
    h.samples = be32(bytes + 0x00);
    h.sampleRate = be32(bytes + 0x08);
    const uint16_t format = (uint16_t)be16(bytes + 0x0E);
    for (int32_t i = 0; i < 16; i++) {
        h.coefs[i] = be16(bytes + 0x1C + i * 2);
    }
    h.predScale = bytes[0x3F];
    h.history0 = be16(bytes + 0x40);
    h.history1 = be16(bytes + 0x42);
    if (format != 0 || h.samples == 0 || h.sampleRate < 8000 || h.sampleRate > 48000) {
        return false;
    }
    *out = h;
    return true;
}

void DspAdpcm::advance(const uint8_t* data, int32_t frames, const int16_t* coefs,
                       DspAdpcmContext* ctx) {
    decode(data, frames, coefs, ctx, nullptr);
}

void DspAdpcm::decode(const uint8_t* data, int32_t frames, const int16_t* coefs,
                      DspAdpcmContext* ctx, int16_t* out) {
    int32_t h1 = ctx->history0;
    int32_t h2 = ctx->history1;
    for (int32_t f = 0; f < frames; f++) {
        ctx->index = data[f * kFrameBytes];
        frame(data + f * kFrameBytes, coefs, &h1, &h2, out ? out + f * kFrameSamples : nullptr);
    }
    ctx->history0 = (int16_t)h1;
    ctx->history1 = (int16_t)h2;
}

} // namespace audio
} // namespace mdk
