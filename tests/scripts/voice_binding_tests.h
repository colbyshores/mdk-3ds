#pragma once

// ============================================================================
// voice_binding_tests.h -- the DSP output's platform-neutral half, pinned.
//
// SPEC.md 8 ("The 3DS follows Forsaken's sound system") moves the summing of the
// voices from the ARM11 to the 3DS DSP and keeps the 64 logical voices shared.
// Three things here decide whether the game can tell the difference, and all
// three are host-testable:
//
//   1. THE LIFECYCLE WITHOUT THE MIX. With the sample plane off
//      (SoundDevice::setSampleMix(false)) a voice's position and end are solved
//      (voice_lifecycle.h), not stepped. Checked against the stepped loop, bit for
//      bit, over random Doppler schedules and the adversarial corners (tiny loops,
//      steps longer than the sound, positions past the end, steps the proof cannot
//      cover). And a whole device with the mix OFF against a twin with the mix ON
//      over the same schedule: the same voices alive after every update, the same
//      positions to the bit, the same ends -- which is what `if_sound_playing` reads.
//   2. THE BINDING (VoiceBinder): allocation, eviction of the oldest claim, an
//      eviction NEVER ending a logical voice (the device it reads is const, and a
//      twin device with no binder ends every voice on the same update), release on
//      end (an ended voice drains, a stopped one is cut), the music pool, the
//      rebind of an evicted looping voice at its logical position, and a replaced
//      device.
//   3. THE MIX THE LAWS GIVE A CHANNEL: the pan law and sampleRate x pitch.
//
// Returns the number of failed checks.
// ============================================================================

namespace mdk {
namespace test {

int runVoiceBindingTests();

} // namespace test
} // namespace mdk
