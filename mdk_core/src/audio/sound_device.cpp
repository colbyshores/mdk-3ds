// ============================================================================
// sound_device.cpp — the voice table and the mix (control + sample plane of
// AudioDevice.cs).
// ============================================================================

#include "audio/sound_device.h"

#include <math.h>
#include <string.h>

namespace mdk {
namespace audio {

namespace {

// AudioDevice.PanRangeDb: DirectSound's pan, the far channel losing up to 100 dB.
const float kPanRangeDb = 100.0f;
// AudioDevice.MusicCutoff: the music is 8-bit at 14-20 kHz and hisses, so the
// filter cuts it above 6 kHz.
const float kMusicCutoff = 6000.0f;
const float kPi = 3.14159265f;

} // namespace

SoundDevice::SoundDevice()
    : musicAlpha_(0.0f), limiter_(kOutputRate) {
    // AddMusic's `alpha = 1f - MathF.Exp(-2f * MathF.PI * MusicCutoff /
    // OutputRate)`, in the C#'s operand order and once instead of per Update.
    /* @vfp-exempt R6 — the music low-pass's coefficient: one expf per device for
     * a constant expression, computed in the constructor rather than per Update
     * as the C# does; the value is the same float (checked bit-for-bit against
     * MathF.Exp, 0x3F131C9A). measured n/a. Verified 2026-11-05. */
    musicAlpha_ = 1.0f - expf(-2.0f * kPi * kMusicCutoff / (float)kOutputRate);
    // AudioDevice.Update's `(int)(OutputRate * QueueSeconds)`, once instead of
    // per Update: 44100 * 0.06f rounds to 2646.0f, so the frame count is 2646.
    queueFrames_ = (int32_t)((float)kOutputRate * kQueueSeconds);
}

void SoundDevice::setBusGain(int32_t bus, float gain) {
    if (bus >= 0 && bus < 2) {
        busGains_[bus] = gain;
    }
}

float SoundDevice::busGain(int32_t bus) const {
    return bus >= 0 && bus < 2 ? busGains_[bus] : 1.0f;
}

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
    voice.sound = sound.sound;
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

void SoundDevice::sample(const Sound& sound, int32_t frame, float* left, float* right) {
    if (sound.channels == 1) {
        const float s = sound.sampleAt(frame);
        *left = s;
        *right = s;
        return;
    }
    *left = sound.sampleAt(frame * 2);
    *right = sound.sampleAt(frame * 2 + 1);
}

// AudioDevice.MixVoice, line for line: linear interpolation between the two
// frames the position falls between, the far channel attenuated by the pan law,
// and false the moment a non-looping voice runs out.
//
// THE POSITION IS A DOUBLE and the index is an `(int)` because that is what the
// oracle does: `voice.Position` is a double accumulated with a float step, and
// `(int)voice.Position` picks the frame. Making the accumulator a float, or the
// index a fixed-point integer, changes which frame each output sample reads and
// therefore changes the samples — which is what golden/L3_audio_mix.txt pins.
bool SoundDevice::mixVoice(Voice& voice, float* out, int32_t frames) {
    const Sound* sound = voice.sound;
    if (!sound || sound->samples.empty() || voice.frames <= 0) {
        // Nothing to play. The lifecycle still has to run, or a voice that the
        // scripts are waiting on would never end: this is what the headless
        // oracle's goldens rely on.
        const float step = voice.pitch * (float)voice.sampleRate / (float)kOutputRate;
        /* @vfp-exempt R3,R4 — the same arithmetic as the pinned loop below, on the
         * branch with nothing to play: the double position (R4) and the float
         * compare that decides the wrap (R3). It is not cold — a voice with no
         * samples still runs its lifecycle for every frame it is alive — and the
         * oracle's own loop is the same shape, so the two branches have to agree.
         * measured n/a (corpus empty). Verified 2026-11-06. */
        for (int32_t f = 0; f < frames; f++) {
            if (voice.position >= (double)voice.frames) {
                if (!voice.looping) {
                    return false;
                }
                voice.position -= (double)voice.frames;
            }
            voice.position += (double)step;
        }
        return true;
    }

    const float step = voice.pitch * (float)voice.sampleRate / (float)kOutputRate;
    /* @vfp-exempt R6 — the pan law's far-channel attenuation, once per voice per
     * update and not per sample: the C# computes it outside its frame loop too.
     * Checked bit-for-bit against MathF.Pow (0x3B4F3E37 at pan 0.5). measured
     * n/a. Verified 2026-11-05. */
    const float far = powf(10.0f, -kPanRangeDb * fabsf(voice.pan) / 20.0f);
    const float left = voice.gain * (voice.pan > 0.0f ? far : 1.0f);
    const float right = voice.gain * (voice.pan < 0.0f ? far : 1.0f);

    /* @vfp-exempt R2,R3,R4 — the three pieces of the oracle's own frame loop, kept
     * because together they choose the samples: the double position (R4), the float
     * compare that decides the wrap (R3) and the (int) truncation that picks the
     * frame (R2). Integerising the accumulator, solving the wrap in closed form, or
     * dropping the interpolation each move which two samples an output frame reads,
     * so they move the mix that golden/L3_audio_mix.txt pins bit for bit. measured
     * n/a (no OG capture exists to measure against). Verified 2026-11-05. */
    for (int32_t f = 0; f < frames; f++) {
        if (voice.position >= (double)voice.frames) {
            if (!voice.looping) {
                return false;
            }
            voice.position -= (double)voice.frames;
        }

        const int32_t i = (int32_t)voice.position;
        const float t = (float)(voice.position - (double)i);
        const int32_t next = i + 1 < voice.frames ? i + 1 : (voice.looping ? 0 : i);

        float l, r, nl, nr;
        sample(*sound, i, &l, &r);
        sample(*sound, next, &nl, &nr);
        out[(size_t)f * 2u] += (l + (nl - l) * t) * left;
        out[(size_t)f * 2u + 1u] += (r + (nr - r) * t) * right;
        voice.position += (double)step;
    }
    return true;
}

// AudioDevice.AddMusic: the music bus through a one-pole low-pass, then both
// buses' volumes into the mix. Note the mix is ASSIGNED, not added, on the way
// out — the effects are multiplied by the effects gain here.
void SoundDevice::addMusic(int32_t frames) {
    const float alpha = musicAlpha_;
    const float effects = busGains_[Effects] * masterGain_;
    const float musicGain = busGains_[Music] * masterGain_;
    for (int32_t f = 0; f < frames; f++) {
        float l = music_[(size_t)f * 2u];
        float r = music_[(size_t)f * 2u + 1u];
        if (musicFilter_) {
            filteredLeft_ = filteredLeft_ + (l - filteredLeft_) * alpha;
            filteredRight_ = filteredRight_ + (r - filteredRight_) * alpha;
            l = filteredLeft_;
            r = filteredRight_;
        }
        mix_[(size_t)f * 2u] = mix_[(size_t)f * 2u] * effects + l * musicGain;
        mix_[(size_t)f * 2u + 1u] = mix_[(size_t)f * 2u + 1u] * effects + r * musicGain;
    }
}

// The frame count to mix. AudioDevice.Update has two answers and picks by whether
// it has a stream:
//
//   frames = _stream == null ? (int)_owed
//                            : (int)(OutputRate * QueueSeconds) - SDL_GetAudioStreamQueued(_stream) / bytesPerFrame;
//   _owed -= _stream == null ? frames : _owed;
//
// The port picks by whether the sink can ANSWER the queue question: SDL can (it is
// the same SDL call), and the muted device and the ndsp sink cannot, so they keep
// the `owed` accounting the golden pins. Both branches are the oracle's, line for
// line; the only thing that moved is which sink is allowed to take which.
void SoundDevice::update(float delta) {
    owed_ += (double)kOutputRate * (double)delta;
    int32_t frames;
    if (sink_ != nullptr && queued_ != nullptr) {
        frames = queueFrames_ - queued_(sinkContext_);
        owed_ = 0.0;
    } else {
        frames = (int32_t)owed_;
        owed_ -= (double)frames;
    }
    if (frames <= 0) {
        return;
    }
    mixedFrames_ = frames;

    if ((size_t)(frames * 2) > mix_.size()) {
        mix_.resize((size_t)(frames * 2));
    }
    if ((size_t)(frames * 2) > music_.size()) {
        music_.resize((size_t)(frames * 2));
    }
    memset(mix_.data(), 0, (size_t)(frames * 2) * sizeof(float));
    memset(music_.data(), 0, (size_t)(frames * 2) * sizeof(float));

    // Removing while enumerating is allowed in the C# and is what this loop
    // does: an ended voice goes and the next one takes its place.
    for (size_t i = 0; i < voices_.size();) {
        Voice& voice = voices_[i];
        float* into = voice.bus == Music ? music_.data() : mix_.data();
        const bool mixed = mixVoice(voice, into, frames);
        if (!mixed) {
            voices_.erase(voices_.begin() + (long)i);
        } else {
            i++;
        }
    }

    addMusic(frames);
    limiter_.process(mix_.data(), frames);

    if (sink_) {
        sink_(sinkContext_, mix_.data(), frames);
    }
}

} // namespace audio
} // namespace mdk
