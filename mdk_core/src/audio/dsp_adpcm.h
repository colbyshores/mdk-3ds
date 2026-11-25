#pragma once

// ============================================================================
// dsp_adpcm.h -- a Nintendo DSP-ADPCM (.dsp) track: its header, and the
// predictor history the 3DS's music stream carries from one wave buffer to the
// next. Pure arithmetic, no I/O, so the host suite pins it.
//
// Adapted from ../t2k (t2k_core/src/audio/music_core.cpp, openDsp and
// adpcmAdvance), itself the Forsaken streamer's kit (vault
// 10-projects/forsaken/findings/dsp-adpcm-music-and-sfx-audio.md).
//
// THE FILE: a 96-byte big-endian header, then 8-byte frames of 14 samples
// (one byte of predictor/scale, seven of nibbles). The header's num_samples
// (offset 0) is the track's length; its num_nibbles (offset 4) is NOT used:
// gc-dspadpcm-encode counts the frame headers in it and other encoders do not.
// The data's length is the file's, less the header, in whole frames.
//
// THE HISTORY. The DSP decodes a wave buffer from the predictor history it is
// handed with it (ndspWaveBuf::adpcm_data). A stream cut into buffers must hand
// each buffer the history the previous buffer ended with, or the predictor
// drifts and the music crackles every few minutes (Forsaken's "broken record").
// advance() decodes a buffer's frames on the CPU only far enough to carry that
// history on; the samples are not kept.
// ============================================================================

#include <stdint.h>

namespace mdk {
namespace audio {

struct DspAdpcmHeader {
    uint32_t samples = 0;     // the track's length in samples
    uint32_t sampleRate = 0;
    int16_t coefs[16] = {0};  // eight predictor pairs
    uint8_t predScale = 0;    // the first frame's header byte
    int16_t history0 = 0;     // the initial predictor history (yn-1)
    int16_t history1 = 0;     // (yn-2)
};

// The predictor state at a frame boundary: what ndspAdpcmData carries.
struct DspAdpcmContext {
    uint16_t index = 0;  // the next frame's header byte
    int16_t history0 = 0;
    int16_t history1 = 0;
};

class DspAdpcm {
public:
    static const int32_t kHeaderBytes = 96;
    static const int32_t kFrameBytes = 8;
    static const int32_t kFrameSamples = 14;

    // Parse the header. False when it is not a mono DSP-ADPCM header this
    // player can stream (rate outside 8-48 kHz, no samples).
    static bool parseHeader(const uint8_t* bytes, int32_t byteCount, DspAdpcmHeader* out);

    // The frames that hold `samples` samples (the last one may be partial).
    static uint32_t framesFor(uint32_t samples) {
        return (samples + (uint32_t)kFrameSamples - 1u) / (uint32_t)kFrameSamples;
    }

    // Carry `ctx` over `frames` frames of `data` (the decoder's arithmetic,
    // samples discarded): on return it is the history after them. `ctx.index`
    // is left at the last frame's header byte.
    static void advance(const uint8_t* data, int32_t frames, const int16_t* coefs,
                        DspAdpcmContext* ctx);

    // Decode `frames` frames into `out` (frames x 14 samples) from `ctx`, which
    // is advanced as advance() does. The host tests' reference decoder.
    static void decode(const uint8_t* data, int32_t frames, const int16_t* coefs,
                       DspAdpcmContext* ctx, int16_t* out);
};

} // namespace audio
} // namespace mdk
