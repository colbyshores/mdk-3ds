#pragma once

// ============================================================================
// audio_sink_tests.h — the sink seam: how much a device mixes, and where it goes.
//
// The mix itself is pinned by golden/L3_audio_mix.txt (MEASURED: the C# device's
// own buffer). What is pinned HERE is the seam around it, and it is
// TRANSLITERATED, because the C# cannot be run with a queue on this host: there
// is no sound device, and the scratch probe that produced the golden ran the
// MUTED device (AudioDevice(Output.Muted)), which is the branch with no queue.
//
// The oracle text is AudioDevice.Update, AudioDevice.cs:214-225:
//
//   _owed += OutputRate * (double)delta;
//   var frames = _stream == null
//       ? (int)_owed
//       : (int)(OutputRate * QueueSeconds) - SDL_GetAudioStreamQueued(_stream) / bytesPerFrame;
//   _owed -= _stream == null ? frames : _owed;
//   if (frames <= 0) return;
//
// with OutputRate = 44100, QueueSeconds = 0.06f, OutputChannels = 2 and
// bytesPerFrame = 2 * sizeof(float) = 8. Every expected number below is computed
// from those constants and cited to that line, not read back from the port.
//
// The port's seam is `setSink(write, context, queued)` where `queued` is the
// SDL_GetAudioStreamQueued half. A sink with no query (the ndsp sink, whose
// library exposes no per-buffer play position) takes the `_stream == null`
// branch, which is the muted device's and the golden's.
//
// The function returns the number of failed checks.
// ============================================================================

namespace mdk {
namespace test {

int runAudioSinkTests();

} // namespace test
} // namespace mdk
