#pragma once

// ============================================================================
// audio_sdl.h — the Linux sink: the finished mix, queued to SDL's default device.
//
// This is the output half of src/Mdk.Engine/Audio/AudioDevice.cs — the
// constructor's `SDL_OpenAudioDeviceStream` and the tail of `Update`'s
// `SDL_PutAudioStreamData` — and nothing else. The mix itself is the shared
// device's (mdk_core/src/audio/sound_device.h); this file only opens a stream,
// pushes what it is handed, and answers how much of it is still queued.
//
// WHY IT IS THIS SHAPE. The C# holds one SDL_AudioStream per AudioDevice and the
// game has one AudioDevice, so a file-static stream here is the same object
// graph, not a shortcut: there is exactly one output per process on this target,
// and the two sink hooks below are the C#'s instance methods with the `this`
// replaced by the context the seam passes.
//
// THE FORMAT IS THE C#'s: SDL_AUDIO_F32LE, two channels, 44100 Hz, no
// conversion callback. The mix is already float stereo at that rate, so the
// stream is a queue and not a converter — which is what lets the shared mix be
// bit-for-bit the oracle's on this target. (The 3DS sink cannot make that
// promise: ndsp wants 16-bit PCM, so the conversion happens there, at the sink.)
//
// NO AUDIO IS NOT AN ERROR. The C# returns from its constructor with `_stream ==
// null` when SDL cannot init audio or there is no device, and every later call
// then takes the muted branch. So does this: audioOpen() returns false, the
// caller leaves the sink unset, and the device mixes and drops exactly as the
// headless oracle does. A machine with no sound card runs the same game.
//
// The stream is opened with SDL's own device callback (NULL), so SDL pulls from
// the queue on its audio thread; nothing here runs on a second thread and the
// queue is SDL's lock to guard.
// ============================================================================

#include <stdint.h>

#include "audio/sound_device.h"

namespace mdk {

// Open the default playback stream. FALSE when there is no audio subsystem or no
// device: the caller then runs the muted device, as the C# does.
bool audioOpen();

// Destroy the stream and quit the audio subsystem (AudioDevice.Dispose). Safe to
// call when nothing was opened.
void audioClose();

// Whether a stream is open (the C#'s `_stream != null`).
bool audioIsOpen();

// The two sink hooks for SoundDevice::setSink — `setSink(audioSink, nullptr,
// audioQueued)`. The context is unused because there is one stream per process.
void audioSink(void* context, const float* interleaved, int32_t frames);
int32_t audioQueued(void* context);

// What the sink has pushed since the stream opened: frames, and the loudest
// sample's magnitude (0 is silence). The run log's evidence that the level's
// voices reach the stream; the mix itself is the device's.
struct AudioStats {
    int64_t frames = 0;
    float peak = 0.0f;
};
AudioStats audioStats();

} // namespace mdk
