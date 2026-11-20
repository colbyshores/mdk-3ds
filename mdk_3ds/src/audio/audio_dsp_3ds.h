#pragma once

// ============================================================================
// audio_dsp_3ds.h -- the 3DS's DSP-mixed output (SPEC.md 8, "The 3DS follows
// Forsaken's sound system"; reference ../forsaken at e8f0774,
// forsaken_3ds/src/audio/sound_3ds.cpp and music_3ds.cpp).
//
// WHAT MOVES AND WHAT DOES NOT. The sound laws and the 64 logical voices stay in
// mdk_core (SoundMixer, SoundDevice), advanced every frame exactly as on the PC
// because the scripts read them (`if_sound_playing`). What this file replaces is
// the OUTPUT: instead of summing the voices into one stereo float stream on the
// ARM11 and handing it to one ndsp channel (audio_3ds.cpp, still compiled for the
// harness's DSPAUDIO=0 A/B), every audible voice plays on its own ndsp channel and
// the DSP mixes them. The device runs with its sample plane OFF
// (SoundDevice::setSampleMix(false)): positions and ends are solved, not stepped,
// so the ARM11 runs no per-sample loop at all.
//
// THE CHANNELS. mdk::audio::VoiceBinder (mdk_core, host-tested) decides which
// logical voice owns which channel; this file only carries its orders out:
//   * effects: ndsp channels 0-7 (Forsaken's MAX_SFX_VOICES), the oldest claim
//     evicted when all are taken, an evicted LOOP rebound when one goes idle;
//   * music: ndsp channel 23 (Forsaken's), and 22 for the outgoing track during
//     LevelMusic's four-second crossfade (the C# sums both on its music bus).
// Volume and pan become the channel's front-left/front-right mix (the shared pan
// law, SoundDevice::panGains, over voice gain x bus gain x master gain); pitch
// times the sound's rate becomes the channel's rate. Linear interpolation, as the
// software mix interpolates.
//
// THE SAMPLES. ndsp reads linear (DSP-visible) memory, and the bank's decoded
// store is the C++ heap. So an effect is copied, the first time it plays, into a
// SAMPLE ARENA reserved ONCE at open (kSampleArenaBytes, one linearAlloc): a
// per-event linearAlloc is what failed on the PD port once textures filled the
// linear heap (vault 10-projects/perfect-dark/findings/dsp-sound-system.md:
// "Allocate DSP memory at boot, never per event"). Entries are first-fit inside
// the arena, keyed by the sound's NAME (one bank, one level: a name is one set of
// samples), least-recently-played evicted when a new one does not fit, never one a
// channel still plays. MDK's 8-bit sounds are stored in the bank as
// (byte-128)*256, so their low byte is always zero; such a sound is put in the
// arena as signed PCM8 (half the bytes, the exact same values). A 16-bit sound
// stays PCM16.
//
// THE MUSIC is streamed, not copied whole: a worker thread (Forsaken's shape:
// 25 ms tick, four wave buffers per channel) copies the next buffer's worth of
// the track's samples out of the bank into linear memory as each buffer comes back
// DONE. The track's PCM is the bank's decoded store (SNI music is PCM; it is NOT
// converted to DSP-ADPCM: that would be a second copy of every track in a new
// format and a new staging step, for a saving the worker does not need). The
// music bus's 6 kHz one-pole low-pass is the music channels' IIR biquad, loaded
// with the one-pole's own coefficients at the DSP's rate (b0 = 1-e, a1 = -e,
// e = exp(-2 pi 6000 / 32728.5)): libctru's biquad custom filter takes RBJ-form
// coefficients (its own LowPass helper passes a0 = 1+alpha, a1 = -2cos w0, ...;
// read off libctru.a's ndsp-filter.o), so a one-pole is a biquad with b1 = b2 =
// a2 = 0. libctru's MONOPOLE low-pass helper is NOT used: it passes a1 = 1-e and
// b0 = e, which on the same reading puts the pole at 1-e (about 2 kHz for a 6 kHz
// request) -- unverified on hardware, and avoided rather than relied on.
//
// NOT CARRIED OVER (SPEC.md 8): the master limiter and the summed stream.
//
// THREADS. The worker follows AGENTS.md R1 (FZ/DN at its entry), P4 (it is
// signalled, woken and joined before anything it reads is freed) and 10 (a small
// stack; no recursion, no large locals). It is created ONCE at open and never on
// resume (P1): an aptHook parks it (under its lock) while the system is
// suspended or asleep, and on resume gives ndsp 100 ms to restore its channels
// before the worker may restart one that looks dry. ndsp itself saves and
// restores the channels across sleep.
// ============================================================================

#include <stdint.h>

#include "audio/sound_bank.h"
#include "audio/sound_mixer.h"

namespace mdk {

// THE A/B SWITCH. A harness build made with `make AUTOQUIT=90 DSPAUDIO=0` keeps the
// software mix (the shared mixer summing on the ARM11 into audio_3ds.cpp's one
// channel), so the console can A/B the two; every other build, release included,
// mixes on the DSP. A build constant (the kDrawDedupe pattern), not a runtime
// toggle: this is not an enhancement, it is the 3DS's sound system.
#if defined(MDK_AUTOQUIT) && defined(MDK_HW_DSPAUDIO_OFF)
static constexpr bool kDspAudio = false;
#else
static constexpr bool kDspAudio = true;
#endif

// ndsp + the sample arena + the music buffers + the worker. FALSE with a logged
// reason (no DSP firmware, ndspInit failed, no linear memory); the game then runs
// muted with the lifecycle still advancing. `bank` is the level's: the music
// streams from it, and the worker's tracks are pinned in it while they play.
bool audioDspOpen(audio::SoundBank* bank);

// Stop and join the worker, silence every channel, ndspExit, free the linear
// memory, and give back the bank pins. Safe when nothing opened. Call while the
// bank is still alive.
void audioDspClose();

bool audioDspIsOpen();

// ONCE A FRAME, between `mix->update(elapsed)` and `mix->device().update(elapsed)`
// (voice_binding.h says why that slot): bind new voices, release ended and
// stopped ones, and hand every bound channel this frame's mix and rate. A no-op
// when the DSP path is not open.
void audioDspFrame(const audio::SoundMixer& mixer);

// Before the device being followed is destroyed (a respawn rebuilds the runtime
// and its mixer): silence everything and forget every binding.
void audioDspDetach();

// The DSP path's ledger (the frontend logs it on the sink ledger's cadence).
struct AudioDspStats {
    uint32_t binds;
    uint32_t evictions;
    uint32_t rebinds;
    uint32_t cuts;
    uint32_t drains;
    int32_t bound;          // channels bound to a live voice now
    uint32_t cacheFills;    // effects copied into the arena
    uint32_t cacheEvictions;
    uint32_t cacheMisses;   // an effect that found no room (it played silent)
    uint32_t arenaUsed;     // bytes of the arena holding samples now
    uint32_t arenaBytes;
    uint32_t musicQueued;   // music wave buffers handed to ndsp
    uint32_t musicRestarts; // drained music channels restarted (an underrun)
    int32_t musicActive;    // music channels streaming now
    uint32_t dspFrames;
    uint32_t dspDropped;
};
AudioDspStats audioDspStats();

} // namespace mdk
