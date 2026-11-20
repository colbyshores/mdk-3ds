#pragma once

// ============================================================================
// voice_lifecycle.h -- a voice's position and end, without its samples.
//
// WHAT A VOICE'S LIFECYCLE IS. SoundDevice::mixVoice steps a voice one output
// frame at a time (AudioDevice.MixVoice):
//
//     for each output frame:
//         if position >= frames:  end (non-looping) or position -= frames (looping)
//         position += step                    // step = pitch * rate / 44100, a float
//
// with `position` a double. That loop decides two things the GAME reads: when a
// non-looping voice ends (the scripts branch on it: `if_sound_playing`, opcode
// 233, ScriptRuntime::isSoundPlaying) and where a looping one is. The samples it
// reads on the way are only what the speaker hears.
//
// WHY THIS FILE EXISTS. On the 3DS the DSP mixes the voices (SPEC.md 8, "The 3DS
// follows Forsaken's sound system"), so the ARM11 no longer reads samples -- but
// the lifecycle must still advance EXACTLY as the mix loop advances it, or a
// voice ends a frame early or late in the scripts' eyes and the gameplay goldens
// move. Running the loop above without its samples is still 735 iterations per
// voice per 1/60 s frame on the ARM11, so lifecycleAdvance() SOLVES it instead
// (AGENTS.md T1) and falls back to the loop whenever it cannot prove the solved
// answer is the loop's answer bit for bit.
//
// THE PROOF THE SOLVED FORM RESTS ON. Let q = 2^e be a power of two that divides
// the position, the step and the sound's frame count (an integer, so e <= 0).
// Every value the loop ever forms -- p + k*step, minus a multiple of frames -- is
// then an integer multiple of q, and while its magnitude stays below 2^53 * q it
// is exactly representable as a double. An add or subtract whose exact result is
// representable is exact under IEEE round-to-nearest, so each `+= step` and
// `-= frames` of the loop is exact, and the loop's position after k steps IS
// p + k*step, which one multiply and one add also compute exactly (k*step is
// itself a multiple of q under the same bound). lifecycleAdvance() reads e off the bit
// patterns of the step (a float) and the position (a double), bounds every value
// the update can reach, and takes the solved path only under that bound; outside
// it it runs lifecycleStep(), the loop itself. Both paths therefore produce the
// same position and the same end, and the host suite drives both over random and
// adversarial schedules to check it (tests/scripts/voice_binding_tests.cpp).
//
// FZ (AGENTS.md R1) flushes SUBNORMAL results; the solved path never forms one
// (its quantum bound keeps every nonzero value far above 2^-1022), and the loop is
// the oracle's own arithmetic.
// ============================================================================

#include <stdint.h>

namespace mdk {
namespace audio {

// The oracle's loop, as SoundDevice::mixVoice's nothing-to-play branch runs it:
// `count` output frames of a voice of `frames` frames at `step`. FALSE once a
// non-looping voice has ended (the position is then where the loop left it).
bool lifecycleStep(double* position, float step, int32_t frames, bool looping, int32_t count);

// The same result, solved: the position and the end lifecycleStep() would give,
// in a handful of operations when the exactness bound holds and by running
// lifecycleStep() when it does not. `*solved` (may be null) says which path ran.
bool lifecycleAdvance(double* position, float step, int32_t frames, bool looping, int32_t count,
                      bool* solved = nullptr);

} // namespace audio
} // namespace mdk
