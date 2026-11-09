// ============================================================================
// mve_audio_tests.cpp — see mve_audio_tests.h.
//
// WHY THIS FILE EXISTS. Batch 95 removed a signed left shift from TWO 8-bit PCM
// decodes — audio/sound.cpp and formats/mve.cpp — and pinned only the first:
// `testSampleStoreExact` walks the whole byte domain through `soundFromPcm`, and
// nothing in tests/ could reach `Mve::decodePcm` at all (review-2 F4 of
// run-20261108-195620). An unpinned half of a fix is the half that comes back,
// and the way it comes back here is a reformatting of `* kByteScale` into
// `<< 8`, which is the undefined behaviour the batch existed to remove.
//
// WHY THE MOVIE IS BUILT HERE AND NOT LOADED. The C# twin of this decoder is
// tested by tests/Mdk.Formats.Tests/MediaTests.cs:30, which calls
// `Mve.Load(Data.PathOf("MISC/FLIC/MDKBZK.MVE"))` — the real end-of-game movie
// from the gitignored game tree. Two reasons that is not the shape of this test:
// the loop's oracle boundary forbids a batch depending on those bytes, and a
// check that needs the data tree SKIPS on a machine without it, which makes it a
// pass generator and not a gate (the argument archive_guard_tests.h makes for
// the same reason). So the container below is synthetic and the ASSERTIONS are
// the oracle's arithmetic, which is the half that has to match.
//
// THE ORACLE, verbatim, is src/Mdk.Formats/Mve.cs:
//
//   :246   samples.Add(step == 2 ? Bin.S16(_bytes, p)
//                                : (_bytes[p] - ByteMiddle) << 8);
//   :258   Audio.Add(Vector2.One * samples[i] * SampleScale);        // mono
//   :266   Audio.Add(new Vector2(samples[i], samples[i + 1]) * SampleScale);
//   :46-47 SampleScale = 1f / 32768f;  ByteMiddle = 128;
//
// The expected value below is written `(b - 128) * 256` and NOT `(b - 128) << 8`.
// That is not a paraphrase of the C#, it is the same value by a definition the
// two languages do not share: C# specifies a signed left shift as a bit shift
// with the high bits discarded, C++17 [expr.shift] leaves it undefined for a
// negative operand, and `(b - 128) * 256` is the value both give for every byte
// (the product spans [-32768, 32512], so nothing overflows int32 or int16). The
// multiply is also exact after the float conversion, because 1/32768 is a power
// of two. That is the whole of batch 95's argument, and this file is where the
// MVE half of it is now pinned.
//
// THE CONTAINER, as formats/mve.cpp actually enforces it:
//
//   offset 0    19 bytes  "Interplay MVE File\x1a" — kSignatureSize is 26, and
//                 the remaining 7 header bytes are skipped, never parsed
//   then chunks  u16 payload size, u16 chunk type, then that many opcode bytes
//   each opcode  u16 data length, u8 type, u8 version, then that many data bytes
//
// so a `length` counts DATA only, not the four-byte opcode header. The two
// opcodes this test needs are 0x03 (init audio: u32 frame time, u16 flags,
// u16 rate) and 0x08 (audio frame: u16 stream id, u16 stream flags, u16
// compressed size, then the samples). With no 0x07 send-buffer opcode the movie
// ends without a shown frame, which is the honest way to ask a decoder for its
// audio and nothing else.
// ============================================================================

#include "mve_audio_tests.h"

#include <stdio.h>
#include <string.h>

#include <vector>

#include "formats/mve.h"

using namespace mdk;

namespace {

int failures = 0;

void expect(const char* what, long long got, long long want) {
    if (got != want) {
        printf("  FAIL: %s was %lld, the oracle has %lld\n", what, got, want);
        failures++;
    }
}

// Bit patterns, not decimals: the claim is that the decode is EXACT, and a
// tolerance would hide the one bit the shift and the multiply could differ in.
void expectBits(const char* what, float got, float want) {
    uint32_t g = 0, w = 0;
    memcpy(&g, &got, sizeof(g));
    memcpy(&w, &want, sizeof(w));
    if (g != w) {
        printf("  FAIL: %s was %08X, the oracle has %08X\n", what, g, w);
        failures++;
    }
}

const char kSignature[] = "Interplay MVE File\x1a";
const int kSignatureSize = 26;   // kSignatureSize in mve.cpp: 19 bytes are compared
const uint8_t kInitAudio = 0x03;
const uint8_t kAudioFrame = 0x08;
const uint8_t kVersion0 = 0;
const int kStreamAudio = 1;      // kStreamMask: this stream carries samples
const int kFlagStereo = 1;       // kAudioStereo
const int kFlag16Bit = 2;        // kAudio16Bit
const float kSampleScale = 1.0f / 32768.0f;   // Mve.cs:46

void put16(std::vector<uint8_t>& to, uint16_t v) {
    to.push_back((uint8_t)(v & 0xFFu));
    to.push_back((uint8_t)(v >> 8));
}

void appendOpcode(std::vector<uint8_t>& chunk, uint8_t type, const std::vector<uint8_t>& data) {
    put16(chunk, (uint16_t)data.size());
    chunk.push_back(type);
    chunk.push_back(kVersion0);
    chunk.insert(chunk.end(), data.begin(), data.end());
}

// One chunk holding an init-audio opcode and one audio-frame opcode.
std::vector<uint8_t> buildMovie(int32_t audioFlags, const std::vector<uint8_t>& pcm) {
    std::vector<uint8_t> init;
    // SIX data bytes, and the fields OVERLAP: run() reads a u32 at +0 for the
    // frame time, then a u16 at +2 for the flags and a u16 at +4 for the rate, so
    // the flags are the upper half of the u32 it just read. That is what the C#
    // does (Mve.cs reads Bin.U32(p), Bin.U16(p+2), Bin.U16(p+4)) and the port
    // follows it; a body laid out as four separate u16 fields silently decodes as
    // "flags 0", which is how this test first failed.
    put16(init, 0);                       // frame time, low half
    put16(init, (uint16_t)audioFlags);    // frame time, high half — and the flags
    put16(init, 22050);                   // sample rate

    std::vector<uint8_t> frame;
    put16(frame, 0);                      // stream id
    put16(frame, (uint16_t)kStreamAudio); // stream flags — bit 0 means "has samples"
    put16(frame, (uint16_t)pcm.size());   // compressed size; unused on the PCM path
    frame.insert(frame.end(), pcm.begin(), pcm.end());

    std::vector<uint8_t> payload;
    appendOpcode(payload, kInitAudio, init);
    appendOpcode(payload, kAudioFrame, frame);

    std::vector<uint8_t> bytes;
    bytes.assign(kSignature, kSignature + strlen(kSignature));
    bytes.resize((size_t)kSignatureSize, 0);
    put16(bytes, (uint16_t)payload.size());
    put16(bytes, 0);                      // chunk type
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

// The oracle's mono sample for one 8-bit byte (Mve.cs:246 + :258).
float oracle8(uint8_t b) { return (float)(((int32_t)b - 128) * 256) * kSampleScale; }

// The oracle's sample for one 16-bit little-endian short (Mve.cs:246 + :258).
float oracle16(int16_t s) { return (float)s * kSampleScale; }

// ---------------------------------------------------------------------------
// The whole byte domain, mono. Every value 0..255 is a separate assertion, so a
// decode that gets the bias or the scale wrong on one part of the domain names
// the byte that failed instead of naming a sum.
// ---------------------------------------------------------------------------
void testMono8BitIsTheWholeByteDomain() {
    std::vector<uint8_t> pcm(256);
    for (int i = 0; i < 256; i++) {
        pcm[(size_t)i] = (uint8_t)i;
    }

    const std::vector<uint8_t> movie = buildMovie(0, pcm);
    formats::Mve mve;
    if (!formats::Mve::load(movie.data(), (int32_t)movie.size(), &mve)) {
        printf("  FAIL: the synthetic movie was not accepted, so nothing below this "
               "line tested the decode\n");
        failures++;
        return;
    }

    // The opcodes are parsed by nextFrame(), not by load(), so every field below
    // is read AFTER the step that reads them. Asserting channels() or sampleRate()
    // before it compares a default against a default and proves nothing.
    expect("the audio opcode ends the movie (no send buffer)",
           (long long)(mve.nextFrame() == formats::Mve::FrameStatus::End), 1);
    expect("the movie parses as mono", mve.channels(), 1);
    // The rate is what proves the init-audio opcode was read at all: with flags 0
    // the decoder takes the PCM branch, and if the opcode had been skipped the
    // audio frame would have decoded into a default-constructed mono/0 Hz movie
    // that still produces samples. Bit depth has no accessor; the branch it
    // selects is what the sample count below shows.
    expect("the init-audio opcode's rate was read", (long long)(mve.sampleRate() == 22050), 1);
    expect("one sample per byte", (long long)mve.audio().size(), 256);
    if (mve.audio().size() != 256) {
        return;
    }

    for (int i = 0; i < 256; i++) {
        const formats::Mve::AudioSample& s = mve.audio()[(size_t)i];
        char label[96];
        snprintf(label, sizeof(label), "byte %d mono left", i);
        expectBits(label, s.left, oracle8((uint8_t)i));
        snprintf(label, sizeof(label), "byte %d mono right (mono duplicates)", i);
        expectBits(label, s.right, oracle8((uint8_t)i));
    }
}

// ---------------------------------------------------------------------------
// The stereo pair order. Every shipped game sound is mono, which is exactly how
// batch 95's stereo bug in the other sink survived: a branch no test reaches.
// The MVE decoder has the same pair, so it is reached here on purpose.
// ---------------------------------------------------------------------------
void testStereo8BitPairsInFileOrder() {
    const uint8_t raw[] = {0, 255, 128, 200, 10, 245};
    const std::vector<uint8_t> pcm(raw, raw + sizeof(raw));

    const std::vector<uint8_t> movie = buildMovie(kFlagStereo, pcm);
    formats::Mve mve;
    if (!formats::Mve::load(movie.data(), (int32_t)movie.size(), &mve)) {
        printf("  FAIL: the synthetic stereo movie was not accepted\n");
        failures++;
        return;
    }

    expect("the audio opcode ends the movie",
           (long long)(mve.nextFrame() == formats::Mve::FrameStatus::End), 1);
    expect("the movie parses as stereo", mve.channels(), 2);
    expect("one sample per PAIR", (long long)mve.audio().size(), 3);
    if (mve.audio().size() != 3) {
        return;
    }

    // Byte order is (left, right) in file order — swapping the pair moves both.
    expectBits("stereo pair 0 left", mve.audio()[0].left, oracle8(0));
    expectBits("stereo pair 0 right", mve.audio()[0].right, oracle8(255));
    expectBits("stereo pair 1 left", mve.audio()[1].left, oracle8(128));
    expectBits("stereo pair 1 right", mve.audio()[1].right, oracle8(200));
    expectBits("stereo pair 2 left", mve.audio()[2].left, oracle8(10));
    expectBits("stereo pair 2 right", mve.audio()[2].right, oracle8(245));
}

// ---------------------------------------------------------------------------
// The 16-bit branch of the same loop, so the branch that was NOT the fix is
// pinned too and a repair aimed at the wrong `step` cannot pass for free.
// ---------------------------------------------------------------------------
void testMono16BitIsTheShortItself() {
    const int16_t shorts[] = {-32768, 0, 32767, 512, -1};
    std::vector<uint8_t> pcm;
    for (size_t i = 0; i < sizeof(shorts) / sizeof(shorts[0]); i++) {
        put16(pcm, (uint16_t)shorts[i]);
    }

    const std::vector<uint8_t> movie = buildMovie(kFlag16Bit, pcm);
    formats::Mve mve;
    if (!formats::Mve::load(movie.data(), (int32_t)movie.size(), &mve)) {
        printf("  FAIL: the synthetic 16-bit movie was not accepted\n");
        failures++;
        return;
    }

    expect("the audio opcode ends the movie",
           (long long)(mve.nextFrame() == formats::Mve::FrameStatus::End), 1);
    expect("one sample per TWO bytes", (long long)mve.audio().size(), 5);
    if (mve.audio().size() != 5) {
        return;
    }

    for (size_t i = 0; i < sizeof(shorts) / sizeof(shorts[0]); i++) {
        char label[96];
        snprintf(label, sizeof(label), "short %d", (int)shorts[i]);
        expectBits(label, mve.audio()[i].left, oracle16(shorts[i]));
    }
}

}  // namespace

namespace mdk {
namespace test {

int runMveAudioTests() {
    failures = 0;
    testMono8BitIsTheWholeByteDomain();
    testStereo8BitPairsInFileOrder();
    testMono16BitIsTheShortItself();
    return failures;
}

}  // namespace test
}  // namespace mdk
