#pragma once

// ============================================================================
// sound_device.h — the voice table the mixer plays into.
//
// THE CONTROL PLANE OF src/Mdk.Engine/Audio/AudioDevice.cs: the id allocator
// (ids from 1, 0 means "no voice"), the 64-voice capacity that drops a new
// sound when every voice is busy, the per-voice gain/pitch/pan a playing voice
// can be re-set to, and the voice lifecycle — a non-looping voice ends once its
// frames have been consumed, a looping one wraps.
//
// WHAT IS NOT HERE. The sample plane — resampling, panning maths, the music
// low-pass, the limiter, and the SDL output stream — belongs to the platform
// backends (SDL on Linux, ndsp on 3DS) and is not ported yet. The lifecycle is
// ported because the SCRIPT state depends on it: `IsVoicePlaying` is what
// if_own_sound (249) reads, and a voice stops being one when the device says it
// has ended. The arithmetic is the muted device's, frame for frame:
// `owed += 44100 * delta` frames, each voice advancing by
// `pitch * sampleRate / 44100` per frame, exactly as MixVoice steps a voice
// that is mixed into nothing.
//
// The headless oracle never calls this update (its driver ticks the scripts
// only), so every voice it starts stays live until it is stopped — which the
// goldens pin. The lifecycle lands with the audio backend's own checks.
//
// Streamed voices (PlayStream/Push, for movie sound) are not ported: nothing in
// the scripts uses them.
// ============================================================================

#include <stdint.h>

#include <vector>

#include "audio/sound_bank.h"

namespace mdk {
namespace audio {

class SoundDevice {
public:
    // AudioDevice: the output rate, and the voice count that drops a new sound.
    static constexpr int32_t kOutputRate = 44100;
    static constexpr int32_t kMaxVoices = 64;

    // The bus a voice mixes into (the music has its own filter).
    enum Bus { Effects, Music };

    // Start a voice: its id, or 0 when all voices are busy.
    int32_t play(const SoundBank::Entry& sound, float gain, float pitch = 1.0f, float pan = 0.0f,
                 int32_t bus = Effects);

    bool isPlaying(int32_t id) const;
    void stop(int32_t id);
    void stopAll();
    void set(int32_t id, float gain, float pitch, float pan);

    // Once a frame: consume `delta` seconds of output and forget what ended.
    void update(float delta);

    int32_t nextId() const { return nextId_; }
    int32_t voiceCount() const { return (int32_t)voices_.size(); }

private:
    struct Voice {
        int32_t id = 0;
        int32_t bus = Effects;
        float gain = 0.0f;
        float pitch = 1.0f;
        float pan = 0.0f;
        double position = 0.0;  // frames into the sound
        int32_t frames = 0;
        int32_t sampleRate = 0;
        bool looping = false;
    };

    // The live voices in play order (the C#'s Dictionary order).
    std::vector<Voice> voices_;
    int32_t nextId_ = 1;
    double owed_ = 0.0;  // output frames owed when there is no device
};

} // namespace audio
} // namespace mdk
