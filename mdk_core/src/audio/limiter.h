#pragma once

// ============================================================================
// limiter.h — the hard limiter on the master mix.
//
// THE C# ORIGINAL is src/Mdk.Engine/Audio/Limiter.cs, a 1:1 port. It models
// Godot's AudioEffectHardLimiter on the Master bus (settings.gd), which is what
// keeps many sounds at once from clipping.
//
// ONE GAIN FOR BOTH CHANNELS, and it is applied to a frame, not a sample: the
// peak of the pair sets the ceiling, so a loud left sample pulls the right one
// down with it. That coupling is the whole design and it is why the limiter runs
// over the finished mix rather than per voice.
//
// IT IS STATEFUL ACROSS FRAMES. `_gain` decays toward 1 by `release` per output
// frame, so a burst ducks the mix and the recovery is heard over the next ~0.1
// s. A limiter constructed fresh each frame would be a compressor with a
// different attack, so the object lives as long as the device.
//
// THE CONSTANTS ARE THE ORACLE'S. `Ceiling` is 10^(-0.5/20) and `release` is
// 1 - e^(-1/(0.1 * rate)), both computed once. Both were checked bit-for-bit
// against the C# on this host (0.944060862 = 0x3F71ADF9 and 0.000226736069 =
// 0x396DC000), so the golden's ceiling and release are the oracle's and not a
// near miss.
// ============================================================================

#include <stdint.h>

namespace mdk {
namespace audio {

class Limiter {
public:
    explicit Limiter(int32_t sampleRate);

    // Limits interleaved stereo samples in place. `frameCount` frames, so
    // frameCount*2 floats.
    void process(float* stereo, int32_t frameCount);

    // The highest sample out (linear, -0.5 dB). Cached in the constructor: the
    // expression is constant and a powf does not belong in the per-frame mix.
    float ceiling() const;

    // For a fresh run (the C#'s field initializer).
    void reset() { gain_ = 1.0f; }

private:
    float ceiling_ = 1.0f;
    float release_ = 0.0f;
    float gain_ = 1.0f;
};

} // namespace audio
} // namespace mdk
