// ============================================================================
// voice_binding.cpp -- see voice_binding.h.
// ============================================================================

#include "audio/voice_binding.h"

namespace mdk {
namespace audio {

void VoiceBinder::reset() {
    for (int32_t c = 0; c < kChannels; c++) {
        Channel& ch = channels_[c];
        const bool busy = ch.voice != 0 || ch.draining;
        ch = Channel();
        ch.cut = busy;
    }
    waitingCount_ = 0;
    lastSeen_ = 0;
    device_ = nullptr;
    adopting_ = true;
}

const SoundDevice::Voice* VoiceBinder::find(const SoundDevice& device, int32_t id) {
    const std::vector<SoundDevice::Voice>& voices = device.voices();
    for (size_t i = 0; i < voices.size(); i++) {
        if (voices[i].id == id) {
            return &voices[i];
        }
    }
    return nullptr;
}

int32_t VoiceBinder::channelOf(int32_t voiceId) const {
    if (voiceId == 0) {
        return -1;
    }
    for (int32_t c = 0; c < kChannels; c++) {
        if (channels_[c].voice == voiceId) {
            return c;
        }
    }
    return -1;
}

bool VoiceBinder::waiting(int32_t voiceId) const {
    for (int32_t i = 0; i < waitingCount_; i++) {
        if (waiting_[i] == voiceId) {
            return true;
        }
    }
    return false;
}

int32_t VoiceBinder::boundCount() const {
    int32_t n = 0;
    for (int32_t c = 0; c < kChannels; c++) {
        n += channels_[c].voice != 0 ? 1 : 0;
    }
    return n;
}

int32_t VoiceBinder::pick(int32_t first, int32_t count, bool evict) const {
    int32_t draining = -1;
    int32_t oldest = -1;
    for (int32_t c = first; c < first + count; c++) {
        const Channel& ch = channels_[c];
        if (ch.voice == 0 && !ch.draining) {
            return c;  // idle: the lowest-numbered idle channel
        }
        if (ch.voice == 0) {
            // Finishing a voice that already ended: the oldest such claim first.
            if (draining < 0 || ch.claimed < channels_[draining].claimed) {
                draining = c;
            }
        } else if (oldest < 0 || ch.claimed < channels_[oldest].claimed) {
            oldest = c;
        }
    }
    if (draining >= 0) {
        return draining;
    }
    return evict ? oldest : -1;
}

void VoiceBinder::remember(int32_t voiceId) {
    if (waitingCount_ < SoundDevice::kMaxVoices) {
        waiting_[waitingCount_++] = voiceId;
    }
}

void VoiceBinder::forgetWaiting(int32_t index) {
    for (int32_t i = index; i + 1 < waitingCount_; i++) {
        waiting_[i] = waiting_[i + 1];
    }
    waitingCount_--;
}

void VoiceBinder::bind(int32_t c, const SoundDevice::Voice& voice, bool rebind) {
    Channel& ch = channels_[c];
    if (ch.voice != 0) {
        // Eviction: the voice keeps playing in the game's eyes, silently. A
        // looping one waits for a channel; a one-shot is let go. (A voice claimed
        // earlier in this same sync never sounded; it is counted all the same.)
        stats_.evictions++;
        if (ch.looping) {
            remember(ch.voice);
        }
    }
    ch.voice = voice.id;
    ch.claimed = ++serial_;
    ch.draining = false;
    ch.cut = false;
    ch.start = true;
    ch.rebind = rebind;
    ch.sound = voice.sound;
    ch.frames = voice.frames;
    ch.sampleRate = voice.sampleRate;
    ch.looping = voice.looping;
    // A fresh voice is seen before its first update, at position 0. A rebound
    // looping voice resumes where the game has it; the position is a double in
    // [0, frames] (it may sit exactly on the wrap the next update makes).
    int32_t frame = 0;
    if (rebind && voice.position > 0.0) {
        // Once per rebind (cold): the frame the DSP resumes the loop at.
        frame = voice.position < (double)voice.frames ? (int32_t)voice.position : 0;
    }
    ch.startFrame = frame;
    ch.mixChanged = true;
}

void VoiceBinder::computeMix(Channel& ch, const SoundDevice& device,
                             const SoundDevice::Voice& voice) {
    float left = 0.0f;
    float right = 0.0f;
    SoundDevice::panGains(voice.gain * device.busGain(voice.bus) * device.masterGain(), voice.pan,
                          &left, &right);
    const float rate = (float)voice.sampleRate * voice.pitch;
    if (ch.start || left != ch.left || right != ch.right || rate != ch.rate) {
        ch.mixChanged = true;
    }
    ch.left = left;
    ch.right = right;
    ch.rate = rate;
}

void VoiceBinder::sync(const SoundDevice& device, const bool* sounding) {
    for (int32_t c = 0; c < kChannels; c++) {
        Channel& ch = channels_[c];
        ch.cut = false;
        ch.start = false;
        ch.rebind = false;
        ch.mixChanged = false;
    }

    // A different device, or one whose ids run backwards (a respawn rebuilt the
    // runtime that owns it at the same address): nothing bound is in it.
    if (&device != device_ || device.nextId() - 1 < lastSeen_) {
        if (device_ != nullptr) {
            reset();
        }
        adopting_ = true;
    }
    device_ = &device;

    // 1. Draining channels that went quiet are idle.
    for (int32_t c = 0; c < kChannels; c++) {
        Channel& ch = channels_[c];
        if (ch.voice == 0 && ch.draining && (sounding == nullptr || !sounding[c])) {
            ch.draining = false;
        }
    }

    // 2. Bound voices that are gone: ended (finish) or stopped (cut).
    for (int32_t c = 0; c < kChannels; c++) {
        Channel& ch = channels_[c];
        if (ch.voice == 0 || find(device, ch.voice) != nullptr) {
            continue;
        }
        if (device.endedInLastUpdate(ch.voice) && !ch.looping) {
            ch.draining = true;
            stats_.drains++;
        } else {
            ch.cut = true;
            stats_.cuts++;
        }
        ch.voice = 0;
    }

    // 3. Waiting voices that are gone are forgotten.
    for (int32_t i = waitingCount_; i-- > 0;) {
        if (find(device, waiting_[i]) == nullptr) {
            forgetWaiting(i);
        }
    }

    // 4. New voices, in play order (ids ascend): each claims a channel of its
    //    pool, evicting the oldest claim when the pool is full.
    const std::vector<SoundDevice::Voice>& voices = device.voices();
    for (size_t i = 0; i < voices.size(); i++) {
        const SoundDevice::Voice& voice = voices[i];
        if (voice.id <= lastSeen_) {
            continue;
        }
        lastSeen_ = voice.id;
        if (voice.sound == nullptr || voice.frames <= 0 || voice.sampleRate <= 0) {
            stats_.silent++;  // nothing to play: the lifecycle runs, nothing sounds
            continue;
        }
        if (adopting_ && voice.position > 0.0) {
            // Under way before this binder saw it (see reset()): a loop waits for
            // a channel, a one-shot is not replayed.
            stats_.adopted++;
            if (voice.looping) {
                remember(voice.id);
            }
            continue;
        }
        const bool music = voice.bus == SoundDevice::Music;
        const int32_t first = music ? kFirstMusicChannel : 0;
        const int32_t count = music ? kMusicChannels : kEffectChannels;
        const int32_t c = pick(first, count, true);
        bind(c, voice, false);
        stats_.binds++;
    }

    // 5. Evicted looping voices come back to IDLE channels only (a draining
    //    channel is still finishing a sound, and a bound one is somebody's), the
    //    longest-waiting first.
    for (int32_t i = 0; i < waitingCount_;) {
        const SoundDevice::Voice* voice = find(device, waiting_[i]);
        const bool music = voice != nullptr && voice->bus == SoundDevice::Music;
        const int32_t first = music ? kFirstMusicChannel : 0;
        const int32_t count = music ? kMusicChannels : kEffectChannels;
        int32_t idle = -1;
        for (int32_t c = first; c < first + count && voice != nullptr; c++) {
            if (channels_[c].voice == 0 && !channels_[c].draining) {
                idle = c;
                break;
            }
        }
        if (idle < 0) {
            i++;
            continue;
        }
        forgetWaiting(i);
        bind(idle, *voice, true);
        stats_.rebinds++;
    }

    adopting_ = false;

    // 6. Every bound channel's mix and rate, from this frame's laws.
    for (int32_t c = 0; c < kChannels; c++) {
        Channel& ch = channels_[c];
        if (ch.voice == 0) {
            continue;
        }
        const SoundDevice::Voice* voice = find(device, ch.voice);
        if (voice != nullptr) {
            computeMix(ch, device, *voice);
        }
    }
}

} // namespace audio
} // namespace mdk
