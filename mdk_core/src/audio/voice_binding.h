#pragma once

// ============================================================================
// voice_binding.h -- which logical voice sounds on which hardware channel.
//
// SPEC.md 8, "The 3DS follows Forsaken's sound system": the sound LAWS and the 64
// logical voices stay shared (SoundMixer / SoundDevice, advanced every frame
// exactly as on the PC, because the scripts read them), and only the OUTPUT
// changes: each audible voice plays on its own hardware channel and the DSP mixes
// them. This class is the platform-neutral half of that output -- which voice
// owns which channel, what each channel must do this frame, and the mix and rate
// the shared laws give it -- so the host suite can pin it. The 3DS backend
// (mdk_3ds/src/audio/audio_dsp_3ds.cpp) turns a channel's orders into ndsp calls
// and owns nothing else of the policy.
//
// THE POOLS. Effects voices share kEffectChannels = 8 channels (ndsp 0-7):
// Forsaken's cap, because the DSP firmware starts skipping audio when too many
// channels are active (vault 10-projects/forsaken/findings/
// dsp-adpcm-music-and-sfx-audio.md B). Music-bus voices get their own
// kMusicChannels = 2, outside that pool: the level's track plays on one and,
// for the four seconds LevelMusic crossfades two tracks, the outgoing one on the
// other (the C# sums both on its music bus).
//
// EVICTION: THE VOICE CLAIMED LONGEST AGO (SPEC.md 8, Forsaken's alloc_channel).
// A new voice takes a channel that is idle, else one that is only finishing a
// voice that already ended, else the one claimed longest ago. The quietest was
// considered: the shared data has each voice's gain, but a gain is a frame's
// value (3D voices move), the C#'s own 64-voice cap drops the NEW sound and not a
// quiet one, and a quiet ambience is exactly the long-lived voice that would then
// be stolen first and never come back. So the order is Forsaken's, and the
// second half of the policy is what keeps ambience alive:
//
// AN EVICTED LOOPING VOICE COMES BACK. Losing a channel only silences a voice; it
// never ends it (the device does that, on its own schedule). A looping voice that
// was evicted is remembered and, the first frame a channel of its pool is IDLE
// (not merely finishing a sound), rebound at its logical position -- so a
// machine hum or an arena's ambient loop drops out during a burst of gunfire and
// returns after it, instead of staying silent for the rest of the level (the PD
// port measured exactly that failure: vault 30-architecture/patterns/
// keep-the-n64-synth-as-a-control-engine-and-play-its-voices-on-hardware.md,
// recipe 3). An evicted one-shot is not brought back: resuming it mid-sound is a
// glitch, not a sound.
//
// WHEN TO CALL sync(): once a frame, AFTER SoundMixer::update (so this frame's
// gain, pitch and pan are set and stopped voices are gone) and BEFORE
// SoundDevice::update (so a voice started this frame is seen at position 0 and is
// started from its first sample, even one too short to survive the update).
// A voice found gone at sync is either ENDED (it ran out in the previous update:
// its channel is left to finish the last frame or so the DSP still holds, and is
// free for reuse) or STOPPED (it is cut now).
//
// No allocation, no virtual dispatch, no callback: a fixed array of channel
// orders that the backend reads after each sync (AGENTS.md 10).
// ============================================================================

#include <stdint.h>

#include "audio/sound_device.h"

namespace mdk {
namespace audio {

class VoiceBinder {
public:
    static constexpr int32_t kEffectChannels = 8;
    static constexpr int32_t kMusicChannels = 2;
    static constexpr int32_t kChannels = kEffectChannels + kMusicChannels;
    // Channels [0, kEffectChannels) are the effects pool, the rest music.
    static constexpr int32_t kFirstMusicChannel = kEffectChannels;

    // One channel: who owns it, and this sync's orders for the backend.
    struct Channel {
        int32_t voice = 0;       // the logical voice bound now (0: none)
        uint32_t claimed = 0;    // claim serial: the smallest is the oldest claim
        bool draining = false;   // free, but still finishing a voice that ended

        // ---- this sync's orders (cleared at the start of every sync) ----
        bool cut = false;        // silence the channel now (no start follows)
        bool start = false;      // (re)start the channel with the fields below
        bool rebind = false;     // the start is an evicted voice coming back
        int32_t startFrame = 0;  // the frame of the sound to start at
        const Sound* sound = nullptr;
        int32_t frames = 0;
        int32_t sampleRate = 0;
        bool looping = false;
        // The mix and the rate, from the shared laws: the pan law over the voice's
        // gain, the bus's gain and the master gain, and sampleRate x pitch. Valid
        // while `voice` != 0; `mixChanged` says they differ from the last sync's.
        float left = 0.0f;
        float right = 0.0f;
        float rate = 0.0f;
        bool mixChanged = false;
    };

    struct Stats {
        uint32_t binds = 0;      // new voices given a channel
        uint32_t evictions = 0;  // bound voices whose channel was taken
        uint32_t rebinds = 0;    // evicted looping voices given a channel back
        uint32_t cuts = 0;       // stopped voices whose channel was cut
        uint32_t drains = 0;     // ended voices left to finish
        uint32_t silent = 0;     // new voices with nothing to play (no samples)
        uint32_t adopted = 0;    // voices under way at a reset, not replayed
    };

    VoiceBinder() { reset(); }

    // Forget every binding: every channel idle, with a `cut` order for each one
    // that was bound or finishing, so the backend silences them. Call it when the
    // device the voices live in is replaced (a respawn rebuilds the runtime and
    // with it its mixer and device). sync() also does it by itself when it is
    // handed a different device or the device's ids run backwards.
    //
    // THE FIRST SYNC AFTER A RESET ADOPTS what the device already holds rather
    // than replaying it: a voice still at position 0 has not sounded and is bound
    // as new, a LOOPING voice already under way waits for a channel like an
    // evicted one (and resumes where the game has it), and a one-shot already
    // under way stays silent -- restarting it from its first sample would play it
    // twice. That matters when a reset leaves the device alive (a respawn that
    // finds no script assets keeps the standalone mixer and its live voices).
    void reset();

    // Bring the channels in line with `device`. `sounding[c]` is whether channel
    // c is still producing sound (ndspChnIsPlaying), read before this call; it
    // only matters for draining channels, which become idle once it is false.
    // Null means "nothing is sounding" (a draining channel is idle at once).
    void sync(const SoundDevice& device, const bool* sounding);

    const Channel& channel(int32_t c) const { return channels_[c]; }
    // The channel a voice sounds on, or -1 (unbound, or evicted).
    int32_t channelOf(int32_t voiceId) const;
    // Whether a voice is evicted and waiting for a channel (looping voices only).
    bool waiting(int32_t voiceId) const;
    int32_t boundCount() const;
    const Stats& stats() const { return stats_; }

private:
    // The live voice with this id, or null.
    static const SoundDevice::Voice* find(const SoundDevice& device, int32_t id);
    // A channel of the pool [first, first + count) for a new claim: idle, else
    // draining, else (if `evict`) the oldest claim; -1 when none qualifies.
    int32_t pick(int32_t first, int32_t count, bool evict) const;
    // Bind `voice` to channel `c` (evicting whatever is there).
    void bind(int32_t c, const SoundDevice::Voice& voice, bool rebind);
    void remember(int32_t voiceId);
    void forgetWaiting(int32_t index);
    void computeMix(Channel& ch, const SoundDevice& device, const SoundDevice::Voice& voice);

    Channel channels_[kChannels];
    // Evicted looping voices, oldest eviction first. At most kMaxVoices are alive.
    int32_t waiting_[SoundDevice::kMaxVoices];
    int32_t waitingCount_ = 0;
    int32_t lastSeen_ = 0;     // the highest voice id this binder has looked at
    const SoundDevice* device_ = nullptr;  // the device the last sync followed
    bool adopting_ = true;     // the next sync is the first since a reset
    uint32_t serial_ = 0;
    Stats stats_;
};

} // namespace audio
} // namespace mdk
