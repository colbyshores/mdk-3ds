#pragma once

// ============================================================================
// audio_3ds.h — the 3DS sink: the finished mix, as PCM16, into ndsp's queue.
//
// The shared mixer (mdk_core/src/audio/) produces interleaved stereo float at
// 44100 Hz, bit-for-bit the C#'s. ndsp wants 16-bit PCM in linear memory, so
// SPEC.md 8 puts the conversion HERE, at the sink, and keeps the mix itself
// shared: "ndsp with a 16-bit PCM stream; the shared mixer's float output is
// converted at the sink. The voice pool shrinks to 24 and the queue to 40 ms to
// fit the 3DS's DSP budget."
//
// ONE CHANNEL, NOT A VOICE POOL. The C# mixes in software and hands SDL a
// finished stereo stream; the 3DS port keeps that design, so ndsp carries the
// MIX, not the voices: one channel (0) with a queue of wave buffers. That is
// also what the console's DSP prefers — the vault's Forsaken finding
// (10-projects/forsaken/findings/dsp-adpcm-music-and-sfx-audio.md §B) is that
// the PICA200 DSP firmware starts SKIPPING audio once too many channels are
// active, and it measured a framerate cost too (60 -> 45 fps). One channel is
// the floor for that cost. The "voice pool shrinks to 24" line of SPEC.md 8 is
// about the SOFTWARE mixer's cap, which is the mixer's own knob, not ndsp's.
//
// THE QUEUE, AND WHY IT IS NOT THE C#'s. SDL can be asked how much it still
// holds (SDL_GetAudioStreamQueued), and the C# uses that to decide how many
// frames to mix. libctru's ndsp exposes no per-buffer play position: a wave
// buffer reports only FREE/QUEUED/PLAYING/DONE. So this sink gives the device no
// queue query, and the device keeps its own `owed += rate*delta` accounting —
// the one golden/L3_audio_mix.txt pins — producing exactly real-time's worth of
// samples per frame while the buffer queue absorbs the jitter. Production and
// consumption are both real time, so the queue neither grows nor starves on
// average; a buffer that is still busy when a fill comes round is DROPPED rather
// than waited for, because blocking the frame on audio is the worse failure.
//
// THE SIZE: 4 buffers of 20 ms = 14,112 bytes of linear memory. SPEC.md 8's
// "40 ms" is the lookahead it asked for; four 20 ms buffers are the same shape
// with twice the headroom, because a level's first frames are slow enough to
// starve a 40 ms queue and an underrun is a click. Measured against the level-3
// load, not guessed: see the progress log for the batch that landed this.
//
// dspfirm. ndspInit() fails with 0xD880A7FA when sdmc:/3ds/dspfirm.cdc is
// missing (the DSP firmware is not redistributable, so it is not in the romfs:
// the same split Forsaken ships). That is not fatal here: audioOpen() reports
// false and the game runs muted, exactly as the C# does with no sound card. The
// reason is logged, because silence with no explanation is a bug report.
//
// THE WORKER THAT IS NOT HERE. Forsaken's music stream needs a background thread
// because it reads a file while the game runs. This sink has nothing to read: the
// mix is already in memory when the frame produces it, so the fill is a
// conversion and a memcpy on the frame's own thread. No worker means no second
// FPSCR state to initialise (AGENTS.md R1) and no small stack to overflow
// (AGENTS.md 10) — both of which the audio worker would have had to carry.
// ============================================================================

#include <stdint.h>

#include "audio/sound_device.h"

namespace mdk {

// dspfirm + ndspInit + channel 0 + the wave-buffer queue. FALSE with a logged
// reason when the console has no DSP firmware or ndsp will not start; the caller
// then leaves the device's sink unset and the game runs muted.
bool audioOpen();

// Stop the channel, free the linear buffers, ndspExit. Safe when nothing opened.
void audioClose();

// Whether the sink is live.
bool audioIsOpen();

// The sink hook for SoundDevice::setSink. No queue query is passed: see the
// header. `setSink(audioSink, nullptr)` then keeps the device's own accounting.
void audioSink(void* context, const float* interleaved, int32_t frames);

} // namespace mdk
