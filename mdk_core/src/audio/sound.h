#pragma once

// ============================================================================
// sound.h — one decoded sound: the samples a voice plays.
//
// THE C# ORIGINAL is the `Sound` class at the top of
// src/Mdk.Engine/Audio/AudioDevice.cs. This is a 1:1 port of it and of
// `Sound.FromPcm`.
//
// WHY IT IS ITS OWN HEADER. The C# declares `Sound` inside AudioDevice.cs and
// both `SoundBank` and `SoundDevice` name it, so a port that put it in
// sound_device.h would make sound_bank.h include the device that includes the
// bank. One header, no cycle.
//
// FLOATS, NOT THE FILE'S BYTES. The oracle converts PCM to float once, at the
// level's load, and every voice after that reads floats: `MixVoice` interpolates
// between two of them per output frame. The conversion is the oracle's, ULP for
// ULP, because it decides the sample values the mix is made of:
//
//   8-bit   unsigned, biased at 128:  (byte - 128) * (1/128)
//   else    16-bit signed little-endian: short * (1/32768)
//
// The 8-bit path is NOT `(byte - 128) / 128.0f`: the C# multiplies by the
// reciprocal constant `ByteScale`, and a divide by 128 is exact here so the two
// agree -- but the 16-bit one is a multiply by `1f/32768f`, and writing a divide
// instead would be a different rounding on a value the golden pins. Both scales
// are therefore constants, as in the C#.
//
// LITTLE-ENDIAN BY HAND. `MemoryMarshal.Cast<byte, short>` reads the host's
// order, which is little-endian on both targets; the explicit byte pair below is
// the same value with no dependency on the host, and it is what the ARM11 does
// anyway (LDRH of a byte-swapped halfword costs an extra instruction, so the
// bytes are combined in registers).
//
// THE MEMORY COST, AND WHY IT IS STILL FLOATS. Measured, not estimated: level 3's
// three archives are 4,390,060 bytes on disk (732952 + 172776 + 3484332) and hold 72
// sounds; decoding every one of them to float is 3,234,976 samples = 12.34 MB,
// because MDK's sounds are mostly 8-bit and a float is four bytes. That is against a
// 3DS with 16 MB of FCRAM, most of which the level's geometry and textures already
// hold. So the ndsp batch cannot simply preload the bank on the console: it has to
// keep the file's 8/16-bit bytes and convert on read, or budget and evict, or stream.
// That is recorded as an open item, not hidden here. The format stays float for the
// one reason that matters for fidelity: the ORACLE's arithmetic is float, and the
// alternative (keep the file's bytes and convert per output frame) moves the
// conversion into the mix loop, which is a per-frame loop of 735 iterations a voice,
// and changes nothing about what is audible.
// ============================================================================

#include <stdint.h>

#include <vector>

namespace mdk {
namespace audio {

// A decoded sound. `samples` is interleaved when there is more than one
// channel (Sound.Channels; every sound in MDK's own archives is mono).
struct Sound {
    std::vector<float> samples;
    int32_t channels = 0;
    int32_t sampleRate = 0;
    bool looping = false;

    // Sound.Frames: samples per channel. Zero channels give zero (the C#
    // would divide by zero here; a decoded sound always has a channel count).
    int32_t frames() const { return channels > 0 ? (int32_t)samples.size() / channels : 0; }
};

// Sound.FromPcm: PCM as 8-bit unsigned or 16-bit signed little-endian.
// `byteCount` is the WAV's `data` chunk; the 16-bit path drops a trailing odd
// byte exactly as `data.Length & ~1` does.
void soundFromPcm(const uint8_t* data, int32_t byteCount, int32_t channels, int32_t sampleRate,
                  int32_t bitsPerSample, bool looping, Sound* out);

} // namespace audio
} // namespace mdk
