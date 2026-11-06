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
// read here as the SOFTWARE mixer's cap, which is the mixer's own knob, not
// ndsp's — and the mixer keeps the oracle's 64 (src/Mdk.Engine/Audio/
// AudioDevice.cs:73, `MaxVoices = 64`, with no platform branch). That reading
// is NOT approved: SPEC.md is owner-protected and §8's sentence says 24, so it
// is an OPEN OWNER QUESTION recorded in docs/CONTINUATION.md, not a settled
// choice. The per-frame cost of 64 software voices on a 268 MHz ARM11 with L2
// off is unmeasured, and nothing in the current build exceeds a handful of
// voices, so it is an unmeasured budget rather than a measured one.
//
// THE QUEUE, AND WHY IT IS NOT THE C#'s. SDL can be asked how much it still
// holds (SDL_GetAudioStreamQueued), and the C# uses that to decide how many
// frames to mix. libctru's ndsp has no "how many frames are still queued"
// query: a wave buffer reports only FREE/QUEUED/PLAYING/DONE, and
// ndspChnGetSamplePos reports the offset inside the ONE buffer now playing, so
// the total still held would have to be rebuilt from that plus this sink's own
// queue depth — a reconstruction, not a measurement, and not done here. So this
// sink gives the device no queue query, and the device keeps its own `owed += rate*delta` accounting —
// the one golden/L3_audio_mix.txt pins — producing exactly real-time's worth of
// samples per frame while the buffer queue absorbs the jitter. Production and
// consumption are both real time, so the queue neither grows nor starves on
// average; a buffer that is still busy when a fill comes round is DROPPED rather
// than waited for, because blocking the frame on audio is the worse failure.
//
// THE SIZE: 4 buffers of 20 ms = 14,112 bytes of linear memory. SPEC.md 8's
// "40 ms" is the lookahead it asked for; four 20 ms buffers are the same shape
// with twice the headroom. That doubling is a DESIGN CHOICE and it was NOT
// measured — an earlier revision of this comment claimed a level-3 load
// measurement that does not exist, and review F2 of run-20261106-035708 caught
// it. The only load number on disk is the emulator's level-7 load frame, 129.5
// ms, which is 6.5 buffers: a frame longer than the queue underruns it by
// construction, because production is proportional to the frame's own elapsed
// time and runs once per frame. No queue depth survives that; only not taking
// 130 ms over one frame does. audioStats() reports the drops, so the next reader
// can check the claim instead of believing it.
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

// WHAT THE SINK HAS ACTUALLY DONE (review F4 of run-20261106-035708). Before
// this existed the 3DS evidence was `sink=1` plus `plays=1`, which proves ndsp
// started and a voice was launched and says NOTHING about whether a wave buffer
// ever reached the DSP, was retired by it, or was dropped — so a wrong nsamples
// unit, a stalled channel or a queue that never drains all read as a working
// sink. These counters make the DSP's consumption rate observable next to the
// production rate: `submitted` is what the mix handed over, `done` is what the
// DSP gave back, `dropped` is what was thrown away because every buffer was
// busy, and `requeued` counts the submits that had to re-configure a drained
// channel. A matched sink sits near dropped=0, requeued=0 with done tracking
// submitted; a queue that backs up shows dropped growing with done well behind
// submitted, and a channel that consumes faster than the mix produces shows
// requeued ~= submitted. dspFrames/dspDropped are ndsp's own counters, which is
// the difference between "the DSP is running" and "our buffers are the ones it
// is running on".
struct AudioStats {
    uint32_t submitted;
    uint32_t done;
    uint32_t dropped;
    uint32_t requeued;
    uint32_t dspFrames;
    uint32_t dspDropped;
    int inDsp;
};
AudioStats audioStats();

// The sink hook for SoundDevice::setSink. No queue query is passed: see the
// header. `setSink(audioSink, nullptr)` then keeps the device's own accounting.
void audioSink(void* context, const float* interleaved, int32_t frames);

} // namespace mdk
