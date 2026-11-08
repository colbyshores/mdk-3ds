#pragma once

// ============================================================================
// sound.h — one decoded sound: the samples a voice plays.
//
// THE C# ORIGINAL is the `Sound` class at the top of
// src/Mdk.Engine/Audio/AudioDevice.cs. This is a port of it and of
// `Sound.FromPcm`, with ONE deliberate difference, recorded in full below: the
// oracle stores float and this store is the file's int16.
//
// WHY IT IS ITS OWN HEADER. The C# declares `Sound` inside AudioDevice.cs and
// both `SoundBank` and `SoundDevice` name it, so a port that put it in
// sound_device.h would make sound_bank.h include the device that includes the
// bank. One header, no cycle.
//
// THE STORE IS int16 AND THE VALUES ARE THE ORACLE'S, BIT FOR BIT. The oracle
// converts PCM to float once, at the level's load, and every voice after that
// reads floats: `MixVoice` interpolates between two of them per output frame.
// This port keeps the int16 the WAV already held and converts at the read, in
// `sampleAt()` — one function, so there is exactly one conversion in the tree
// and one place to look for its cost.
//
// The conversion is exact, not close, because BOTH of the oracle's scales are
// powers of two and every intermediate is an integer:
//
//   16-bit  the file's short is stored as it is, and `sampleAt` computes
//           `(float)short * (1/32768)` — the oracle's own expression, with the
//           oracle's own constant, moved from the decode to the read.
//   8-bit   unsigned, biased at 128, stored pre-scaled by 256 as
//           `(byte - 128) << 8`. For byte in [0,255] that spans [-32768,
//           32512], so it fits int16 exactly; reading it back gives
//           `(byte-128) * 2^8 * 2^-15` = `(byte-128) * 2^-7` = the oracle's
//           `(byte - 128) * (1/128)`.
//
// Both paths are dyadic arithmetic on a numerator that needs four bits, so no
// rounding happens on either side and the two are the SAME float, not a near
// miss. That is what makes this a store change and not a fidelity change, and
// the claim is pinned rather than argued: golden/L3_audio_mix.txt (the oracle's
// own mix buffer, 40 updates, both PCM widths, both buses, both loop states)
// and golden/L3_audio_meta.txt (per-sound decoded length, a double sum over
// every sample, and four named sample bit patterns) are unchanged across it.
//
// WHY THE 8-BIT PATH IS NOT `(byte - 128) / 128.0f`. The C# multiplies by the
// reciprocal constant `ByteScale`, and a divide by 128 is exact here so the two
// agree -- but the 16-bit one is a multiply by `1f/32768f`, and writing a divide
// instead would be a different rounding on a value the golden pins. Both scales
// are therefore constants, as in the C#, and only one of them is still applied
// at runtime: `kShortScale`, at the read. The byte scale survives as the shift
// that puts a byte into the same int16 domain, which is the same number.
//
// LITTLE-ENDIAN BY HAND. `MemoryMarshal.Cast<byte, short>` reads the host's
// order, which is little-endian on both targets; the explicit byte pair in
// sound.cpp is the same value with no dependency on the host, and it is what the
// ARM11 does anyway (LDRH of a byte-swapped halfword costs an extra
// instruction, so the bytes are combined in registers).
//
// THE MEMORY, MEASURED, AND WHAT HALVING IT BUYS. Measured on both builds with
// `tick1 mixsize <level>` (it preloads the bank and reads the element size out
// of the store, so the line is the store's width and not a comment about it):
// level 3's three archives are 4,390,060 bytes on disk and hold 72 sounds =
// 3,234,976 samples, which is 12,939,904 bytes as float and 6,469,952 as int16.
// Level 4 is 15,757,072 -> 7,878,536 and level 8 is 20,004,168 -> 10,002,084,
// against a 3DS with 16 MB of FCRAM most of which the level's geometry and
// textures already hold. So the store change halves the largest single audio
// allocation the port makes — and it does NOT make `preload()` safe: the SNI
// archives stay resident too (4.39 / 5.90 / 5.82 MB for the same three levels),
// so a fully decoded level 3 still costs 10.86 MB and level 8 15.83 MB before a
// single triangle. Budget-and-evict, or dropping the archive bytes once a sound
// has decoded, or streaming, stay open; what changed is that the decoded store is
// no longer the term that alone exceeds FCRAM on every level.
//
// THE COST, AND IT IS A HOT-PATH COST. A voice's mix loop is hot path by
// AGENTS.md 2's second criterion (735 output frames a frame, no capture needed),
// and it reads two samples per output frame, so the store moves one `vcvt.f32.s32`
// and one `vmul` per read into that loop — four extra float ops per output frame
// per voice, where the interpolation itself is four. That is a trade made for
// memory and not for speed, and it is pinned at `sampleAt()` under R8 rather than
// hidden in a comment. Nothing calls update() from a shipped frontend on the
// console yet, so it is an unmeasured cost by a profile today, exactly as the
// loop it sits in is.
// ============================================================================

#include <stdint.h>

#include <vector>

namespace mdk {
namespace audio {

// The oracle's ShortScale (AudioDevice.cs), and now the one scale the store is
// read through. Exact: 32768 is a power of two, so this is an exponent shift
// and not a division.
constexpr float kShortScale = 1.0f / 32768.0f;

// A decoded sound. `samples` is interleaved when there is more than one
// channel (Sound.Channels; every sound in MDK's own archives is mono).
struct Sound {
    // The sample store's type, named so the bank can state its footprint in
    // terms of the store instead of spelling a width out in a comment.
    typedef std::vector<int16_t> Samples;

    // The samples, in the file's own int16 domain: the 16-bit path is the file's
    // short unchanged, the 8-bit path is `(byte - 128) << 8`. Read them with
    // sampleAt(); nothing else in the tree should scale them.
    Samples samples;
    int32_t channels = 0;
    int32_t sampleRate = 0;
    bool looping = false;

    // One sample as the oracle's float. The conversion is the oracle's own
    // multiply by 1/32768 and it is exact for both PCM widths — see the header.
    float sampleAt(int32_t index) const {
        /* @vfp-exempt R8 — the store is the file's int16 and the mix is the
         * oracle's float, so every read promotes fixed point back to float.
         * That promotion is the store's purpose (it halves the bank) and it is
         * exact for both widths: both scales are powers of two, so no sample
         * value moves. measured n/a. Verified 2026-11-06. */
        return (float)samples[(size_t)index] * kShortScale;
    }

    // Sound.Frames: samples per channel. Zero channels give zero (the C#
    // would divide by zero here; a decoded sound always has a channel count).
    int32_t frames() const { return channels > 0 ? (int32_t)samples.size() / channels : 0; }
};

// The store's width is the reason the store exists: level 3's 3,234,976 decoded
// samples are 6,469,952 bytes here and were 12,939,904 as float, against 16 MB of
// FCRAM (measured by `tick1 mixsize 3`, and see this header's memory section for
// what halving it does and does not buy). Widening the store is allowed only with a
// new measurement of that trade, so it fails the build rather than passing tests and
// quietly doubling the bank.
static_assert(sizeof(Sound::Samples::value_type) == 2,
              "the sample store is int16 on purpose: it halves the bank against "
              "FCRAM and the conversion at the read is exact for both PCM widths "
              "(see sound.h). Widening it doubles level 3's bank from 6.47 MB to "
              "12.94 MB — measure that trade before changing it.");

// Sound.FromPcm: PCM as 8-bit unsigned or 16-bit signed little-endian.
// `byteCount` is the WAV's `data` chunk; the 16-bit path drops a trailing odd
// byte exactly as `data.Length & ~1` does.
void soundFromPcm(const uint8_t* data, int32_t byteCount, int32_t channels, int32_t sampleRate,
                  int32_t bitsPerSample, bool looping, Sound* out);

} // namespace audio
} // namespace mdk
