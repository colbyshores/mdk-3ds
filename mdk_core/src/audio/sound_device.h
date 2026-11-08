#pragma once

// ============================================================================
// sound_device.h — the voice table the mixer plays into, and the mix it makes.
//
// THE CONTROL PLANE of src/Mdk.Engine/Audio/AudioDevice.cs: the id allocator
// (ids from 1, 0 means "no voice"), the 64-voice capacity that drops a new
// sound when every voice is busy, the per-voice gain/pitch/pan a playing voice
// can be re-set to, and the voice lifecycle — a non-looping voice ends once its
// frames have been consumed, a looping one wraps. The frame count is the muted
// device's, frame for frame: `owed += 44100 * delta` frames, each voice
// advancing by `pitch * sampleRate / 44100` per frame, exactly as MixVoice steps
// a voice.
//
// THE SAMPLE PLANE is what `mix()` hands out: the voices resampled and panned
// into a stereo float buffer, the music bus through its one-pole low-pass, the
// bus and master volumes, and the limiter. It is ported HERE, in mdk_core and
// not in the platform backends, for the reason the whole port runs on: the C# is
// the oracle, the C# mixes in software, and a mix that lives in a platform
// backend cannot be pinned against it. `golden/L3_audio_mix.txt` is the C#
// device's own mix buffer — read out of the live device by the scratch probe and
// digested, never recomputed by the probe — and it is the only audio evidence this
// host can produce at all: there is no sound device here, and the emulator's
// output cannot be heard or captured from this session.
//
// WHAT THE BACKENDS OWN is one thing: what happens to the finished buffer. The
// sink seam below is that seam — plain function pointers and a context, because
// AGENTS.md 10 refuses std::function. SDL on Linux queues it to an audio stream;
// ndsp on 3DS copies it into a wave buffer. Neither of them changes a sample, and
// that is the point of the seam.
//
// HOW MANY FRAMES TO MIX is the one thing the seam cannot keep one-way, and the
// C# says so: `Update` mixes `2646 - SDL_GetAudioStreamQueued(stream)/8` frames
// when it has a stream and `(int)_owed` when it has none (AudioDevice.cs:217-221).
// So the seam has a SECOND, optional half: a `queued` query that answers how many
// output frames the backend still holds. A sink that has one (SDL) gets the C#'s
// top-up accounting; a sink that does not (ndsp, which exposes no per-buffer play
// position in libctru) keeps the device's own `owed += rate*delta` accounting —
// the one `golden/L3_audio_mix.txt` pins — and lets its buffer queue absorb the
// frame jitter. Both branches are the oracle's; neither is invented here.
//
// WHERE THE ENUMERATION ORDER COMES FROM, AND ITS LIMIT. The C# sums its voices in
// the enumeration order of a `Dictionary<int, Voice>` it mutates while enumerating,
// and .NET documents that order as unspecified; a removed slot is reused by the next
// `Play`, while this vector erases and appends. Float addition is not associative,
// so a schedule in which a voice ENDS before a later `Play` mixes the same voices in
// a different order and the samples differ — measured by the batch-79 review at 52
// of 120 updates differing on the golden's own schedule with a stop, a peak delta of
// 1.79e-07 against a 0.944 peak (-134 dBFS) and up to 6698 ULP on a single sample.
// That is inaudible and it is not fixable without pinning .NET's slot layout, which
// is not the oracle's behaviour either; see the note at the golden in
// tests/scripts/tick1.cpp. A voice that is explicitly `stop()`ed has always been in
// the golden's schedule and is unaffected: the difference is only in the order the
// SURVIVING voices are summed in.
//
// The headless oracle never calls this update (its driver ticks the scripts
// only), so every voice it starts stays live until it is stopped — which the
// script goldens pin. The mix has its own driver and its own golden.
//
// Streamed voices (PlayStream/Push, for movie sound) are not ported: nothing in
// the scripts uses them.
//
// HOT PATH, BY THE CONTRACT'S OWN ARITHMETIC. A voice's mix loop runs
// `44100 * delta` = 735 iterations a frame, and AGENTS.md 2's SECOND criterion
// makes anything that runs 256+ times a frame hot path with no capture at all: the
// corpus is only what the first criterion (top 15 of a profile) needs, and the
// empty corpus waives the staleness clocks, not the classification. So these loops
// are hot, and the oracle's arithmetic inside them is fidelity-gated. The double
// position, the `(int)` frame index it truncates to, and the `position >= frames`
// predicate together decide which two samples every output frame interpolates
// between, so integerising the accumulator or solving the wrap in closed form moves
// the samples the golden pins. Each site carries its pin for that reason, with
// `measured n/a` because there is no OG capture to measure against; a fixed-point
// redesign is a proposal for after the first capture and a fidelity verdict, not a
// cleanup. One more op arrives with the int16 sample store (sound.h): each of the
// two sample reads per output frame is now a `vcvt.f32.s32` and a `vmul` on top of
// the load, pinned at `Sound::sampleAt()` under R8 because it is a fixed-point
// domain promoted back to float mid-loop and it is exact, not approximate. What
// that promotion costs per read is FIVE ops and not two, and the middle one is a
// boundary crossing by this contract's own definition: the shipped mono path —
// `SoundDevice::sample`, inlined into `mixVoice` at 0x103864 in this build — reads
//     lsl / ldrsh / vmov s,r / vcvt.f32.s32 / vmul.f32
// and the ARM -> VFP `vmov` is the transfer AGENTS.md §1 prices at 1 cycle
// throughput and 4 cycles latency, with the conversion's 8-cycle latency on top.
// Nothing calls update() from a shipped frontend yet — the ndsp batch
// wires it — so none of this is a measured cost today; it is the site list,
// written down before the code that would need it is optimised.
// ============================================================================

#include <stdint.h>

#include <vector>

#include "audio/limiter.h"
#include "audio/sound_bank.h"
#include "audio/sound.h"

namespace mdk {
namespace audio {

class SoundDevice {
public:
    // AudioDevice: the output rate, and the voice count that drops a new sound.
    static constexpr int32_t kOutputRate = 44100;
    static constexpr int32_t kMaxVoices = 64;

    // The bus a voice mixes into (the music has its own filter).
    enum Bus { Effects, Music };

    // The music low-pass's coefficient and the limiter are both constants of the
    // output rate, so they are set up once here.
    SoundDevice();

    // THE SINK. `frames` interleaved stereo frames at kOutputRate, the finished
    // mix, called once per update() that mixed anything. The buffer belongs to
    // the device and is only good until the next update, so a backend that needs
    // it longer copies it. NULL is the muted device: the mix is computed and
    // dropped, exactly as the C# does when it has no SDL stream.
    typedef void (*Sink)(void* context, const float* interleaved, int32_t frames);
    // How many output frames the sink still holds and has not played. NULL (the
    // muted device and the ndsp sink) means "no queue to ask": the device then
    // mixes the delta it was given, which is the golden's accounting.
    typedef int32_t (*Queued)(void* context);
    void setSink(Sink sink, void* context, Queued queued = nullptr) {
        sink_ = sink;
        sinkContext_ = context;
        queued_ = queued;
    }

    // The C#'s QueueSeconds: how much output the C# keeps queued ahead of the
    // play head, 60 ms, and the frame count it derives from it (44100 * 0.06f =
    // 2646). Only a sink that can answer the queue question uses it; the 3DS's
    // 40 ms from SPEC.md 8 is realised as its wave-buffer queue instead (see
    // mdk_3ds/src/audio/audio_3ds.h), so there is no per-target knob here.
    static constexpr float kQueueSeconds = 0.06f;
    int32_t queueFrames() const { return queueFrames_; }

    // Start a voice: its id, or 0 when all voices are busy.
    int32_t play(const SoundBank::Entry& sound, float gain, float pitch = 1.0f, float pan = 0.0f,
                 int32_t bus = Effects);

    bool isPlaying(int32_t id) const;
    void stop(int32_t id);
    void stopAll();
    void set(int32_t id, float gain, float pitch, float pan);

    // Once a frame: consume `delta` seconds of output, mix it, and forget what
    // ended.
    void update(float delta);

    int32_t nextId() const { return nextId_; }
    int32_t voiceCount() const { return (int32_t)voices_.size(); }

    // ---- The mix's volumes, and the finished buffer (AudioDevice's public set) ----

    // The volume of everything (0-1), and of one bus.
    void setMasterGain(float gain) { masterGain_ = gain; }
    float masterGain() const { return masterGain_; }
    void setBusGain(int32_t bus, float gain);
    float busGain(int32_t bus) const;
    // The music's low-pass on or off (AudioDevice.MusicFilter; On is the C#'s
    // default and the one the golden runs with).
    void setMusicFilter(bool on) { musicFilter_ = on; }

    // The last update()'s mix: interleaved stereo, mixedFrames() frames.
    const float* mix() const { return mix_.empty() ? nullptr : mix_.data(); }
    int32_t mixedFrames() const { return mixedFrames_; }

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
        // The samples, owned by the SoundBank the voice was played from. Null
        // for a voice with nothing to play: the lifecycle still runs, which is
        // what the script goldens depend on.
        const Sound* sound = nullptr;
    };

    // MixVoice: one voice into `out` (interleaved stereo, `frames` frames).
    // FALSE once the voice has ended, exactly as the C# returns false.
    static bool mixVoice(Voice& voice, float* out, int32_t frames);
    // One frame of a sound, both channels.
    static void sample(const Sound& sound, int32_t frame, float* left, float* right);
    // AddMusic: the music bus, filtered, into the mix, with the volumes.
    void addMusic(int32_t frames);

    // The live voices in play order (the C#'s Dictionary order).
    std::vector<Voice> voices_;
    int32_t nextId_ = 1;
    double owed_ = 0.0;  // output frames owed when there is no device

    // The mix and the music bus, both interleaved stereo, grown to the largest
    // frame count ever asked for (AudioDevice._mix and _music).
    std::vector<float> mix_;
    std::vector<float> music_;
    int32_t mixedFrames_ = 0;
    float masterGain_ = 1.0f;
    float busGains_[2] = {1.0f, 1.0f};
    bool musicFilter_ = true;
    // The music low-pass's coefficient and its state. The coefficient is a
    // constant of the output rate, so it is computed once here instead of once
    // per Update as the C# does; the value is the same float.
    float musicAlpha_;
    float filteredLeft_ = 0.0f;
    float filteredRight_ = 0.0f;
    Limiter limiter_;

    Sink sink_ = nullptr;
    void* sinkContext_ = nullptr;
    Queued queued_ = nullptr;
    int32_t queueFrames_ = 2646;  // (int)(44100 * kQueueSeconds), computed in the ctor
};

} // namespace audio
} // namespace mdk
