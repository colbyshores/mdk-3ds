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
//           `(byte - 128) * 256`. For byte in [0,255] that spans [-32768,
//           32512], so it fits int16 exactly; reading it back gives
//           `(byte-128) * 2^8 * 2^-15` = `(byte-128) * 2^-7` = the oracle's
//           `(byte - 128) * (1/128)`. A multiply and not a shift, because the
//           biased value is negative for half the byte domain and C++17 makes a
//           signed left shift UB there (sound.cpp).
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
// Level 4 is 15,757,072 -> 7,878,536 and level 8 is 20,004,168 -> 10,002,084.
// The SNI archives stay resident after a decode too (4,390,060 / 5,904,992 /
// 5,821,432 for the same three levels), so a FULLY decoded bank is 10,860,012 /
// 13,783,528 / 15,823,516 bytes — 10.86 / 13.78 / 15.82 MB, decimal — before a
// single triangle. Halving the store halves the largest single audio allocation
// the port makes; it does not bound it, and `preload()` is still not called on
// the console (main_3ds.cpp).
//
// WHAT IT HAS TO FIT IN, AND THE NUMBER THIS FILE USED TO GET WRONG. This header
// used to say "against a 3DS with 16 MB of FCRAM". That figure is not a fact
// about this console and it is gone; review-1 F3 of run-20261108-195620 found
// it, and it had been quietly driving the priority order. The three numbers that
// ARE sourced, in the order of how much they actually bound the port:
//
//   1. What THIS BUILD ASKS FOR, and this one is measured out of the shipped
//      artifact rather than asserted: the crt0 header at vaddr 0x100010 holds
//      `__heap_size = 0x01800000` (24 MiB) and `__linear_heap_size =
//      0x02000000` (32 MiB). Those are devkitARM's `3dsx_crt0.o` defaults;
//      `mdk_3ds/Makefile` overrides neither, so the C++ heap that `new` and
//      every `std::vector` in the port — the sample bank included — grows
//      inside is 24 MiB. Reproduce:
//      `arm-none-eabi-objdump -s --start-address=0x100010 --stop-address=0x100018 mdk_3ds/mdk.elf`.
//   2. What the CONSOLE HAS: libctru documents 128 MiB of linear FCRAM mapping
//      on an Old 3DS (`OS_OLD_FCRAM_SIZE 0x8000000`, `3ds/os.h:49`) and 256 MiB
//      on a New 3DS (`OS_FCRAM_SIZE`, `3ds/os.h:72`). SPEC.md 7.7, which is
//      owner text, states the project's own budget as "6 MB of VRAM and 64 MB of
//      RAM (32 MB of "system" heap on OG)". Neither is 16 MB.
//   3. What the KERNEL ACTUALLY GRANTS, which is the only one of the three that
//      is a ceiling and is STILL UNMEASURED: `osGetMemRegionSize(MEMREGION_APPLICATION)`
//      reads `OS_KernelConfig->memregion_sz[0]` (`3ds/os.h:219`), a runtime
//      value, not a constant. Nothing in this repository has ever printed it, on
//      hardware or in the emulator, and a .3dsx request is not a grant.
//
// So the honest reading is the opposite of the one this header carried: on the
// numbers that ARE sourced, level 8's fully decoded BANK (15.82 MB) fits inside
// the 24 MiB the build asks for, and 9.34 MB of that request is left over. The
// second term of that sentence is the one that is NOT measured: nothing in this
// repository measures what a level's own geometry, textures, sprites and BSP
// need, so 9.34 MB is what is LEFT FOR them and not evidence they fit in it
// (review-2 F3 of run-20261108-195620). The sentence this replaces said "level 8
// fits", which is a verdict about a level drawn from a measurement about a bank.
// Budget-and-evict, or freeing the archive bytes once a sound has decoded, or
// streaming, stay open for the reasons that survive the correction: nothing
// bounds the bank's growth, the same heap holds the level itself, and the granted
// region has never been read. Do not re-derive a fit-or-no-fit verdict from a
// memory figure that is not one of these three.
//
// THE COST, AND IT IS A HOT-PATH COST. A voice's mix loop is hot path by
// AGENTS.md 2's second criterion (735 output frames a frame, no capture needed),
// and it reads two samples per output frame, so the store moves the whole
// conversion into that loop. The shipped sequence for the mono path MDK actually
// uses — `objdump -d mdk.elf`, inside `SoundDevice::mixVoice` at 0x103864, where
// `SoundDevice::sample` is inlined in this build — is FIVE ops per read and not
// two:
//
//     lsl   r3, r3, #1         frame -> byte offset
//     ldrsh r3, [r5, r3]       the int16 load
//     vmov  s13, r3            ARM -> VFP register transfer
//     vcvt.f32.s32 s13, s13    int -> float
//     vmul.f32 s13, s13, s9    by 2^-15
//
// The `vmov` is not a detail: an ARM -> VFP transfer is its own boundary crossing
// in AGENTS.md §1's table (1 cycle throughput, 4 cycles latency), and the
// conversion is another (8 cycles latency), so a read is an address shift, a
// load, a core crossing, a conversion and a multiply. Two reads per output frame
// per voice is ten of those where the interpolation itself is four. That is a
// trade made for memory and not for speed, and it is pinned at `sampleAt()` under
// R8 rather than hidden in a comment. The addresses and the exact shape are this
// build's, not a permanent property: `sample()` is inlined here and was a separate
// function in the build that introduced the store, and the text section moved
// 0x57b70 -> 0x57ba0 (+48 B) across the change that inlined it. Nothing calls
// update() from a shipped frontend on the console yet, so none of it is a measured
// cost by a profile today, exactly as the loop it sits in is.
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
    // short unchanged, the 8-bit path is `(byte - 128) * 256`. Read them with
    // sampleAt(); nothing else in the tree should scale them.
    Samples samples;
    int32_t channels = 0;
    int32_t sampleRate = 0;
    bool looping = false;

    // One sample as the oracle's float. The conversion is the oracle's own
    // multiply by 1/32768 and it is exact for both PCM widths — see the header.
    // The index is the store's own type: a stereo pair is formed in it, so the
    // pair's arithmetic cannot overflow (sound_device.cpp, review-1 F4).
    float sampleAt(size_t index) const {
        /* @vfp-exempt R8 — the store is the file's int16 and the mix is the
         * oracle's float, so every read promotes fixed point back to float.
         * That promotion is the store's purpose (it halves the bank) and it is
         * exact for both widths: both scales are powers of two, so no sample
         * value moves. measured n/a. Verified 2026-11-08. */
        return (float)samples[(size_t)index] * kShortScale;
    }

    // Sound.Frames: samples per channel. Zero channels give zero (the C#
    // would divide by zero here; a decoded sound always has a channel count).
    int32_t frames() const { return channels > 0 ? (int32_t)samples.size() / channels : 0; }
};

// The store's width is the reason the store exists: level 3's 3,234,976 decoded
// samples are 6,469,952 bytes here and were 12,939,904 as float (measured by
// `tick1 mixsize 3`, and see this header's memory section for what halving it does
// and does not buy, and for which memory budget is sourced and which is not). The
// claim this assert pins is the halving, not a fit against a ceiling: widening the
// store is allowed only with a new measurement of that trade, so it fails the
// build rather than passing tests and quietly doubling the bank.
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
