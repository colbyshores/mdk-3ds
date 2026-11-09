// ============================================================================
// mve_audio_tests.cpp — see mve_audio_tests.h.
//
// WHAT IT COVERS. Both halves of the MVE audio decode: the uncompressed PCM path
// (both widths, mono and stereo) and the INTERPLAY DPCM path — the delta table,
// the clamp, the per-channel predictors, the stereo alternation, the per-chunk
// re-seed — plus the silence opcode and the two broken-size shapes. Batch 96
// added the PCM half and left the compressed half untested, which review-3 of
// run-20261108-195620 named as its open question 2: "the MVE compressed (DPCM)
// path and SilenceFrame are still unreached by any test ... 'the MVE audio
// decode' now reads as covered when half of it is not." That gap is closed here,
// and it paid for itself the first time it was run: the size guard in
// decodeAudio is a heap-buffer-overflow pin, below.
//
// WHY THIS FILE EXISTS AT ALL. Batch 95 removed a signed left shift from TWO
// 8-bit PCM decodes — audio/sound.cpp and formats/mve.cpp — and pinned only the
// first:
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
// and for the compressed path:
//
//   :50-66 short[] Deltas = [ ... 256 entries ... ];
//   :222-226  for (ch) { _predictors[ch] = Bin.S16(_bytes, p);
//                        samples[count++] = _predictors[ch]; p += 2; }
//   :228-234  while (count < samples.Length && p < end) {
//                 _predictors[channel] = Math.Clamp(_predictors[channel]
//                                       + Deltas[_bytes[p]],
//                                       short.MinValue, short.MaxValue);
//                 samples[count++] = _predictors[channel];
//                 p++; channel ^= Channels - 1; }
//   :176-179  SilenceFrame: Audio.AddRange(Repeat(Vector2.Zero,
//                        Bin.U16(_bytes, p + 4) / (2 * Channels)));
//   :143-145  InitAudio: if (version == 0) _audioFlags &= ~AudioCompressed;
//
// The delta table below is transcribed from :50-66 and is a GOLDEN, not a copy
// of the implementation's: it is the oracle's own 256 numbers, and the sweep
// feeds every one of them through the real decoder, so a single edited entry in
// formats/mve.cpp's kDeltas names itself.
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
// so a `length` counts DATA only, not the four-byte opcode header. The opcodes
// this test needs are 0x03 (init audio: two unread bytes, u16 flags at +2, u16
// rate at +4), 0x08 (audio frame: u16 stream id, u16 stream flags, u16 declared
// size, then the samples) and 0x09 (silence: the same three u16s and no body).
// With no 0x07 send-buffer opcode the movie ends without a shown frame, which is
// the honest way to ask a decoder for its audio and nothing else.
//
// THE VERSION BYTE IS NOT DECORATIVE. Opcode 0x03 with version 0 clears the
// compressed flag whatever the flags field says (Mve.cs:143-145, mve.cpp:157),
// so a DPCM fixture MUST put 1 in the version byte or it silently exercises the
// PCM path and passes. That is the one way this file could have covered the
// compressed decode while testing the uncompressed one.
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
const uint8_t kSilenceFrame = 0x09;
const uint8_t kVersion0 = 0;
const uint8_t kVersion1 = 1;     // the only version that keeps the compressed flag
const int kStreamAudio = 1;      // kStreamMask: this stream carries samples
const int kFlagStereo = 1;       // kAudioStereo
const int kFlag16Bit = 2;        // kAudio16Bit
const int kFlagCompressed = 4;   // kAudioCompressed: Interplay DPCM
const float kSampleScale = 1.0f / 32768.0f;   // Mve.cs:46

void put16(std::vector<uint8_t>& to, uint16_t v) {
    to.push_back((uint8_t)(v & 0xFFu));
    to.push_back((uint8_t)(v >> 8));
}

void appendOpcode(std::vector<uint8_t>& chunk, uint8_t type, uint8_t version,
                  const std::vector<uint8_t>& data) {
    put16(chunk, (uint16_t)data.size());
    chunk.push_back(type);
    chunk.push_back(version);
    chunk.insert(chunk.end(), data.begin(), data.end());
}

// One audio-frame opcode: the size the u16 DECLARES and the bytes that follow it.
// They are separate on purpose. The declared size is `count * 2` and the payload
// is 2 seed bytes per channel plus one delta byte per remaining sample, so a
// movie can declare more samples than its payload can supply — which is the
// broken-data shape the guard test below exists for.
struct AudioChunk {
    int32_t declaredSize = 0;
    std::vector<uint8_t> payload;
};

// The DPCM body the oracle consumes: one seed short per channel, then one delta
// byte per remaining sample (Mve.cs:222-234).
std::vector<uint8_t> dpcmBody(const std::vector<int16_t>& seeds,
                              const std::vector<uint8_t>& deltas) {
    std::vector<uint8_t> body;
    for (size_t i = 0; i < seeds.size(); i++) {
        put16(body, (uint16_t)seeds[i]);
    }
    body.insert(body.end(), deltas.begin(), deltas.end());
    return body;
}

// One chunk: an init-audio opcode and then one audio-frame opcode per chunk.
std::vector<uint8_t> buildAudioMovie(int32_t audioFlags, uint8_t initVersion,
                                     const std::vector<AudioChunk>& chunks) {
    std::vector<uint8_t> init;
    // SIX data bytes, of which the FIRST TWO ARE UNREAD PADDING. The opcode this
    // fixture emits is 0x03 (init audio), and it takes the flags from +2 and the
    // rate from +4 (mve.cpp:154,164 — Mve.cs:148,153 read Bin.U16(p+2) and
    // Bin.U16(p+4)). The u32 at +0 belongs to a DIFFERENT init opcode, 0x02
    // (create timer: Bin.U32(p) and Bin.U16(p+4), Mve.cs:139), which this movie
    // does not emit. So +0/+1 exist only because the flags are not at +0; a body
    // laid out as three packed u16 fields decodes as "flags 0" and that is how
    // this test first failed. (review-3 F2 of run-20261108-195620: an earlier
    // comment here called the flags "the upper half of the frame-time u32", a
    // read opcode 0x03 never performs.)
    put16(init, 0);                       // unread padding — NOT a frame time
    put16(init, (uint16_t)audioFlags);    // flags, at +2
    put16(init, 22050);                   // sample rate, at +4

    std::vector<uint8_t> payload;
    appendOpcode(payload, kInitAudio, initVersion, init);
    for (size_t i = 0; i < chunks.size(); i++) {
        std::vector<uint8_t> frame;
        put16(frame, 0);                      // stream id
        put16(frame, (uint16_t)kStreamAudio); // stream flags — bit 0 means "has samples"
        put16(frame, (uint16_t)chunks[i].declaredSize);
        frame.insert(frame.end(), chunks[i].payload.begin(), chunks[i].payload.end());
        appendOpcode(payload, kAudioFrame, kVersion0, frame);
    }

    std::vector<uint8_t> bytes;
    bytes.assign(kSignature, kSignature + strlen(kSignature));
    bytes.resize((size_t)kSignatureSize, 0);
    put16(bytes, (uint16_t)payload.size());
    put16(bytes, 0);                      // chunk type
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

// The PCM shape the first three tests use: one chunk whose declared size is the
// payload itself, and version 0 (which cannot select the compressed path).
std::vector<uint8_t> buildMovie(int32_t audioFlags, const std::vector<uint8_t>& pcm) {
    AudioChunk chunk;
    chunk.declaredSize = (int32_t)pcm.size();
    chunk.payload = pcm;
    std::vector<AudioChunk> chunks;
    chunks.push_back(chunk);
    return buildAudioMovie(audioFlags, kVersion0, chunks);
}

// A movie whose only sound opcode is 0x09, a silence of `byteCount` bytes.
std::vector<uint8_t> buildSilenceMovie(int32_t audioFlags, int32_t byteCount) {
    std::vector<uint8_t> init;
    put16(init, 0);
    put16(init, (uint16_t)audioFlags);
    put16(init, 22050);

    std::vector<uint8_t> frame;
    put16(frame, 0);
    put16(frame, (uint16_t)kStreamAudio);
    put16(frame, (uint16_t)byteCount);

    std::vector<uint8_t> payload;
    appendOpcode(payload, kInitAudio, kVersion1, init);
    appendOpcode(payload, kSilenceFrame, kVersion0, frame);

    std::vector<uint8_t> bytes;
    bytes.assign(kSignature, kSignature + strlen(kSignature));
    bytes.resize((size_t)kSignatureSize, 0);
    put16(bytes, (uint16_t)payload.size());
    put16(bytes, 0);
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

// ---------------------------------------------------------------------------
// The compressed (DPCM) half. The oracle's delta table, transcribed from
// Mve.cs:50-66 — a golden, i.e. the oracle's own numbers and not a copy of
// formats/mve.cpp's kDeltas, which is the thing under test.
// ---------------------------------------------------------------------------
const int16_t kOracleDeltas[256] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 47, 51, 56, 61,
    66, 72, 79, 86, 94, 102, 112, 122, 133, 145, 158, 173, 189, 206, 225, 245,
    267, 292, 318, 348, 379, 414, 452, 493, 538, 587, 640, 699, 763, 832, 908, 991,
    1081, 1180, 1288, 1405, 1534, 1673, 1826, 1993, 2175, 2373, 2590, 2826, 3084, 3365, 3672, 4008,
    4373, 4772, 5208, 5683, 6202, 6767, 7385, 8059, 8794, 9597, 10472, 11428, 12471, 13609, 14851, 16206,
    17685, 19298, 21060, 22981, 25078, 27367, 29864, 32589, -29973, -26728, -23186, -19322, -15105, -10503, -5481, -1,
    1, 1, 5481, 10503, 15105, 19322, 23186, 26728, 29973, -32589, -29864, -27367, -25078, -22981, -21060, -19298,
    -17685, -16206, -14851, -13609, -12471, -11428, -10472, -9597, -8794, -8059, -7385, -6767, -6202, -5683, -5208, -4772,
    -4373, -4008, -3672, -3365, -3084, -2826, -2590, -2373, -2175, -1993, -1826, -1673, -1534, -1405, -1288, -1180,
    -1081, -991, -908, -832, -763, -699, -640, -587, -538, -493, -452, -414, -379, -348, -318, -292,
    -267, -245, -225, -206, -189, -173, -158, -145, -133, -122, -112, -102, -94, -86, -79, -72,
    -66, -61, -56, -51, -47, -43, -42, -41, -40, -39, -38, -37, -36, -35, -34, -33,
    -32, -31, -30, -29, -28, -27, -26, -25, -24, -23, -22, -21, -20, -19, -18, -17,
    -16, -15, -14, -13, -12, -11, -10, -9, -8, -7, -6, -5, -4, -3, -2, -1,
};

// Loads a movie and runs it to its end. `wantBroken` is the FrameStatus the step
// must land on: End for a movie that decoded, Broken for one that must be
// refused. A container the loader rejected is a FAIL and not a skip, because
// every assertion after it would then be reading a default-constructed decoder.
bool decodeAll(const std::vector<uint8_t>& movie, formats::Mve* out, const char* label,
               bool wantBroken) {
    if (!formats::Mve::load(movie.data(), (int32_t)movie.size(), out)) {
        printf("  FAIL: %s: the synthetic movie was not accepted, so nothing below this "
               "line tested the decode\n", label);
        failures++;
        return false;
    }
    const long long want = wantBroken ? (long long)formats::Mve::FrameStatus::Broken
                                      : (long long)formats::Mve::FrameStatus::End;
    expect(label, (long long)out->nextFrame(), want);
    return true;
}

// One mono DPCM movie: a seed short and then one delta byte, two samples out.
std::vector<uint8_t> dpcmMovieMono(int16_t seed, uint8_t delta, int32_t declaredSize) {
    AudioChunk chunk;
    chunk.declaredSize = declaredSize;
    std::vector<int16_t> seeds;
    seeds.push_back(seed);
    std::vector<uint8_t> deltas;
    deltas.push_back(delta);
    chunk.payload = dpcmBody(seeds, deltas);
    std::vector<AudioChunk> chunks;
    chunks.push_back(chunk);
    return buildAudioMovie(kFlagCompressed, kVersion1, chunks);
}

// ---------------------------------------------------------------------------
// Every entry of the delta table, through the real decoder. One movie per index
// so a wrong entry names itself; the seed is 0 and every entry is inside
// [-32589, 32589], so the clamp cannot interfere with any of them and the second
// sample IS the table entry.
// ---------------------------------------------------------------------------
void testDeltasAreTheOracleTable() {
    for (int d = 0; d < 256; d++) {
        const std::vector<uint8_t> movie = dpcmMovieMono(0, (uint8_t)d, 4);
        formats::Mve mve;
        char label[96];
        snprintf(label, sizeof(label), "delta %d movie", d);
        if (!decodeAll(movie, &mve, label, false)) {
            return;
        }
        snprintf(label, sizeof(label), "delta %d sample count", d);
        expect(label, (long long)mve.audio().size(), 2);
        if (mve.audio().size() != 2) {
            return;
        }
        snprintf(label, sizeof(label), "delta %d seed sample", d);
        expectBits(label, mve.audio()[0].left, 0.0f);
        snprintf(label, sizeof(label), "delta table entry %d", d);
        expectBits(label, mve.audio()[1].left, (float)kOracleDeltas[d] * kSampleScale);
    }
}

// ---------------------------------------------------------------------------
// Mono accumulation: the predictor is the previous OUTPUT, not the previous
// delta, and with one channel `channel ^= channels_ - 1` is `^= 0`, i.e. every
// delta lands on the same predictor. A decoder that reset the predictor per
// sample, or that added the delta to the seed each time, fails here.
// ---------------------------------------------------------------------------
void testDpcmMonoAccumulatesInOneChannel() {
    AudioChunk chunk;
    chunk.declaredSize = 8;                          // four samples
    std::vector<int16_t> seeds;
    seeds.push_back(1000);
    std::vector<uint8_t> deltas;
    deltas.push_back(1);   // +1
    deltas.push_back(43);  // +43
    deltas.push_back(255); // -1
    chunk.payload = dpcmBody(seeds, deltas);
    std::vector<AudioChunk> chunks;
    chunks.push_back(chunk);

    const std::vector<uint8_t> movie = buildAudioMovie(kFlagCompressed, kVersion1, chunks);
    formats::Mve mve;
    if (!decodeAll(movie, &mve, "mono dpcm movie", false)) {
        return;
    }
    expect("four samples", (long long)mve.audio().size(), 4);
    if (mve.audio().size() != 4) {
        return;
    }

    const int32_t want[4] = {1000, 1001, 1044, 1043};
    for (int i = 0; i < 4; i++) {
        char label[64];
        snprintf(label, sizeof(label), "mono dpcm sample %d", i);
        expectBits(label, mve.audio()[i].left, (float)want[i] * kSampleScale);
        snprintf(label, sizeof(label), "mono dpcm sample %d duplicated to right", i);
        expectBits(label, mve.audio()[i].right, (float)want[i] * kSampleScale);
    }
}

// ---------------------------------------------------------------------------
// The clamp, at BOTH ends. Mve.cs:230 clamps to short.MinValue/short.MaxValue,
// i.e. [-32768, 32767]; a clamp to 32767 on the low side, or to the int range,
// or no clamp at all, each gives a different sample. Index 119 is the table's
// +32589 and index 137 its -32589, the only entries big enough to reach the
// boundary from a seed inside the short range.
// ---------------------------------------------------------------------------
void testDpcmClampsToTheShortRange() {
    const int16_t seeds[2] = {30000, -30000};
    const uint8_t indexes[2] = {119, 137};
    const int32_t wants[2] = {32767, -32768};

    for (int i = 0; i < 2; i++) {
        const std::vector<uint8_t> movie = dpcmMovieMono(seeds[i], indexes[i], 4);
        formats::Mve mve;
        char label[96];
        snprintf(label, sizeof(label), "clamp %d movie", i);
        if (!decodeAll(movie, &mve, label, false)) {
            return;
        }
        snprintf(label, sizeof(label), "clamp %d sample count", i);
        expect(label, (long long)mve.audio().size(), 2);
        if (mve.audio().size() != 2) {
            return;
        }
        snprintf(label, sizeof(label), "clamp %d seed survives unclamped", i);
        expectBits(label, mve.audio()[0].left, (float)seeds[i] * kSampleScale);
        snprintf(label, sizeof(label), "clamp %d lands on the short bound", i);
        expectBits(label, mve.audio()[1].left, (float)wants[i] * kSampleScale);
    }
}

// ---------------------------------------------------------------------------
// Stereo DPCM: two seeds, then the deltas ALTERNATE, starting from channel 0
// (Mve.cs:233 `channel ^= Channels - 1` with channel starting at 0), and each
// channel accumulates on its OWN predictor. Swapping the pair, restarting the
// alternation, or sharing one predictor between the channels each fails here.
// ---------------------------------------------------------------------------
void testDpcmStereoSeedsBothAndAlternates() {
    AudioChunk chunk;
    chunk.declaredSize = 12;                         // six samples
    std::vector<int16_t> seeds;
    seeds.push_back(100);
    seeds.push_back(-100);
    std::vector<uint8_t> deltas;
    deltas.push_back(1);
    deltas.push_back(2);
    deltas.push_back(3);
    deltas.push_back(5);
    chunk.payload = dpcmBody(seeds, deltas);
    std::vector<AudioChunk> chunks;
    chunks.push_back(chunk);

    const std::vector<uint8_t> movie =
        buildAudioMovie(kFlagStereo | kFlagCompressed, kVersion1, chunks);
    formats::Mve mve;
    if (!decodeAll(movie, &mve, "stereo dpcm movie", false)) {
        return;
    }
    expect("the movie is stereo", mve.channels(), 2);
    expect("three PAIRS", (long long)mve.audio().size(), 3);
    if (mve.audio().size() != 3) {
        return;
    }

    const int32_t left[3] = {100, 101, 104};
    const int32_t right[3] = {-100, -98, -93};
    for (int i = 0; i < 3; i++) {
        char label[64];
        snprintf(label, sizeof(label), "stereo dpcm pair %d left", i);
        expectBits(label, mve.audio()[i].left, (float)left[i] * kSampleScale);
        snprintf(label, sizeof(label), "stereo dpcm pair %d right", i);
        expectBits(label, mve.audio()[i].right, (float)right[i] * kSampleScale);
    }
}

// ---------------------------------------------------------------------------
// Each audio-frame opcode RE-SEEDS its predictors from its own bytes
// (Mve.cs:222-226 is inside DecodeAudio, not outside it). The second chunk here
// seeds 7 after a first chunk that ended on 501; a decoder that kept the member
// predictor and ignored the seed would emit 502 and 503.
// ---------------------------------------------------------------------------
void testDpcmReseedsFromEachChunk() {
    std::vector<AudioChunk> chunks;

    AudioChunk first;
    first.declaredSize = 4;
    std::vector<int16_t> seedA;
    seedA.push_back(500);
    std::vector<uint8_t> deltaA;
    deltaA.push_back(1);
    first.payload = dpcmBody(seedA, deltaA);
    chunks.push_back(first);

    AudioChunk second;
    second.declaredSize = 4;
    std::vector<int16_t> seedB;
    seedB.push_back(7);
    std::vector<uint8_t> deltaB;
    deltaB.push_back(1);
    second.payload = dpcmBody(seedB, deltaB);
    chunks.push_back(second);

    const std::vector<uint8_t> movie = buildAudioMovie(kFlagCompressed, kVersion1, chunks);
    formats::Mve mve;
    if (!decodeAll(movie, &mve, "two-chunk dpcm movie", false)) {
        return;
    }
    expect("both chunks decoded", (long long)mve.audio().size(), 4);
    if (mve.audio().size() != 4) {
        return;
    }

    const int32_t want[4] = {500, 501, 7, 8};
    for (int i = 0; i < 4; i++) {
        char label[64];
        snprintf(label, sizeof(label), "chunk sample %d", i);
        expectBits(label, mve.audio()[i].left, (float)want[i] * kSampleScale);
    }
}

// ---------------------------------------------------------------------------
// Opcode 0x03 with version 0 has no compressed sound whatever the flags say
// (Mve.cs:143-145). This is the rule that decides WHICH decode runs, so it is
// also the rule a DPCM fixture can get wrong and then certify the PCM path by
// accident. Flags say compressed, version says 0, and the bytes are plain 8-bit
// PCM: the answer is four samples, not the two a DPCM decode would produce.
// ---------------------------------------------------------------------------
void testVersionZeroClearsCompression() {
    const uint8_t raw[] = {128, 255, 0, 64};
    AudioChunk chunk;
    chunk.declaredSize = 4;
    chunk.payload = std::vector<uint8_t>(raw, raw + sizeof(raw));
    std::vector<AudioChunk> chunks;
    chunks.push_back(chunk);

    const std::vector<uint8_t> movie = buildAudioMovie(kFlagCompressed, kVersion0, chunks);
    formats::Mve mve;
    if (!decodeAll(movie, &mve, "version-0 movie", false)) {
        return;
    }
    expect("version 0 decoded the payload as PCM", (long long)mve.audio().size(), 4);
    if (mve.audio().size() != 4) {
        return;
    }
    for (int i = 0; i < 4; i++) {
        char label[64];
        snprintf(label, sizeof(label), "version-0 sample %d is the 8-bit PCM value", i);
        expectBits(label, mve.audio()[i].left, oracle8(raw[i]));
    }
}

// ---------------------------------------------------------------------------
// Opcode 0x09 (silence): the count is the declared BYTES over 2 * channels, so
// the divisor is the frame size and not a channel count, and the division
// truncates. Mono 8 -> 4 frames, mono 5 -> 2 (not 3), stereo 8 -> 2.
// ---------------------------------------------------------------------------
void testSilenceFrameCountIsBytesOverFrameSize() {
    struct Case {
        const char* label;
        int32_t flags;
        int32_t bytes;
        int32_t frames;
    };
    const Case cases[] = {
        {"mono silence of 8 bytes", 0, 8, 4},
        {"mono silence of 5 bytes truncates", 0, 5, 2},
        {"stereo silence of 8 bytes", kFlagStereo, 8, 2},
        {"stereo silence of 2 bytes is nothing", kFlagStereo, 2, 0},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const std::vector<uint8_t> movie = buildSilenceMovie(cases[i].flags, cases[i].bytes);
        formats::Mve mve;
        if (!decodeAll(movie, &mve, cases[i].label, false)) {
            return;
        }
        expect(cases[i].label, (long long)mve.audio().size(), cases[i].frames);
        for (size_t s = 0; s < mve.audio().size(); s++) {
            expectBits("silence left is zero", mve.audio()[s].left, 0.0f);
            expectBits("silence right is zero", mve.audio()[s].right, 0.0f);
        }
    }
}

// ---------------------------------------------------------------------------
// THE HEAP-OVERFLOW PIN. A compressed audio frame whose declared size cannot
// hold one predictor per channel used to write predictors_ past the end of the
// sample array: `count = size / 2` sizes it, and the seed loop fills `channels_`
// slots before any loop condition consults `count`. Measured before the guard:
//
//   mono,   declared 1 -> UBSan "reference binding to null pointer of type
//                         'value_type'" at mve.cpp's samples[written++]
//   stereo, declared 2 -> AddressSanitizer "heap-buffer-overflow ... WRITE of
//                         size 4" at mve.cpp:281
//
// The C# cannot do this: `new int[size / 2]` then `samples[count++] = ...`
// raises IndexOutOfRange, and mve.h documents that this port answers that throw
// with FrameStatus::Broken. So Broken IS the oracle's behaviour here, and the
// shapes below are the oracle's answer, not a hardening invented to make a
// sanitizer quiet. The mono case is also caught by ubsan_decode_test.sh, which
// compiles this file under -fsanitize=undefined: delete the guard and the
// sanitizer gate redds as well as this one.
// ---------------------------------------------------------------------------
void testDpcmSizeBelowThePredictorsIsBroken() {
    struct Case {
        const char* label;
        int32_t flags;
        int32_t declaredSize;
        int32_t payloadBytes;
    };
    const Case cases[] = {
        {"mono dpcm declaring 1 byte has no room for its predictor", 0, 1, 4},
        {"mono dpcm declaring 0 bytes has no room for its predictor", 0, 0, 4},
        {"stereo dpcm declaring 2 bytes has room for one of two predictors", kFlagStereo, 2, 6},
        {"stereo dpcm declaring 3 bytes has room for one of two predictors", kFlagStereo, 3, 6},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        AudioChunk chunk;
        chunk.declaredSize = cases[i].declaredSize;
        chunk.payload.assign((size_t)cases[i].payloadBytes, 0x40);
        std::vector<AudioChunk> chunks;
        chunks.push_back(chunk);

        const std::vector<uint8_t> movie =
            buildAudioMovie(cases[i].flags | kFlagCompressed, kVersion1, chunks);
        formats::Mve mve;
        if (!decodeAll(movie, &mve, cases[i].label, true)) {
            return;
        }
        expect("a refused frame decodes no audio", (long long)mve.audio().size(), 0);
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
    testDeltasAreTheOracleTable();
    testDpcmMonoAccumulatesInOneChannel();
    testDpcmClampsToTheShortRange();
    testDpcmStereoSeedsBothAndAlternates();
    testDpcmReseedsFromEachChunk();
    testVersionZeroClearsCompression();
    testSilenceFrameCountIsBytesOverFrameSize();
    testDpcmSizeBelowThePredictorsIsBroken();
    return failures;
}

}  // namespace test
}  // namespace mdk
