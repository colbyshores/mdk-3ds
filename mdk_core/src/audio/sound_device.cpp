// ============================================================================
// sound_device.cpp — the voice table (control plane of AudioDevice.cs).
// ============================================================================

#include "audio/sound_device.h"

namespace mdk {
namespace audio {

int32_t SoundDevice::play(const SoundBank::Entry& sound, float gain, float pitch, float pan,
                          int32_t bus) {
    if ((int32_t)voices_.size() >= kMaxVoices) {
        return 0;
    }

    Voice voice;
    voice.id = nextId_++;
    voice.bus = bus;
    voice.gain = gain;
    voice.pitch = pitch;
    voice.pan = pan;
    voice.position = 0.0;
    voice.frames = sound.frames;
    voice.sampleRate = sound.sampleRate;
    voice.looping = sound.looping;
    voices_.push_back(voice);
    return voice.id;
}

bool SoundDevice::isPlaying(int32_t id) const {
    for (size_t i = 0; i < voices_.size(); i++) {
        if (voices_[i].id == id) {
            return true;
        }
    }
    return false;
}

void SoundDevice::stop(int32_t id) {
    for (size_t i = 0; i < voices_.size(); i++) {
        if (voices_[i].id == id) {
            voices_.erase(voices_.begin() + (long)i);
            return;
        }
    }
}

void SoundDevice::stopAll() { voices_.clear(); }

void SoundDevice::set(int32_t id, float gain, float pitch, float pan) {
    for (size_t i = 0; i < voices_.size(); i++) {
        if (voices_[i].id == id) {
            voices_[i].gain = gain;
            voices_[i].pitch = pitch;
            voices_[i].pan = pan;
            return;
        }
    }
}

// The muted device's frame count: `owed += 44100 * delta`, and the whole
// accumulated seconds are mixed (there is no stream queue to top up).
void SoundDevice::update(float delta) {
    owed_ += (double)kOutputRate * (double)delta;
    const int32_t frames = (int32_t)owed_;
    owed_ -= (double)frames;
    if (frames <= 0) {
        return;
    }

    for (size_t i = 0; i < voices_.size();) {
        Voice& voice = voices_[i];
        // MixVoice's step: float arithmetic, accumulated into a double position.
        const float step = voice.pitch * (float)voice.sampleRate / (float)kOutputRate;
        bool ended = false;
        for (int32_t f = 0; f < frames; f++) {
            if (voice.position >= (double)voice.frames) {
                if (!voice.looping) {
                    ended = true;
                    break;
                }
                voice.position -= (double)voice.frames;
            }
            voice.position += (double)step;
        }
        if (ended) {
            voices_.erase(voices_.begin() + (long)i);
        } else {
            i++;
        }
    }
}

} // namespace audio
} // namespace mdk
