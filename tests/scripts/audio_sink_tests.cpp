// ============================================================================
// audio_sink_tests.cpp — see audio_sink_tests.h.
// ============================================================================

#include "audio_sink_tests.h"

#include <stdio.h>
#include <string.h>

#include "audio/sound_mixer.h"

using namespace mdk;

namespace {

int failures = 0;

void expect(const char* what, long long got, long long want) {
    if (got != want) {
        printf("  FAIL: %s was %lld, the oracle has %lld\n", what, got, want);
        failures++;
    }
}

// Compare two floats as bit patterns. A decimal comparison would round away the
// only thing being pinned: the store change is exact or it is nothing, and "it
// matches to 6 digits" is not the claim.
void expectBits(const char* what, float got, float want) {
    uint32_t g = 0, w = 0;
    memcpy(&g, &got, sizeof(g));
    memcpy(&w, &want, sizeof(w));
    if (g != w) {
        printf("  FAIL: %s was %08X, the oracle has %08X\n", what, g, w);
        failures++;
    }
}

// ---------------------------------------------------------------------------
// A sink that records what it was handed, and answers the queue question from a
// field the test sets. This is the SDL stream's stand-in: SDL_GetAudioStreamQueued
// is a number the backend owns, so a fake that returns a number the test chose is
// the same shape, and the C# line being pinned is the arithmetic around it.
// ---------------------------------------------------------------------------
struct Probe {
    int calls;
    int32_t frames;
    float firstSample;
    int32_t queued;  // what the queue query answers
};

Probe g_probe;

void probeSink(void* context, const float* interleaved, int32_t frames) {
    Probe* p = (Probe*)context;
    p->calls++;
    p->frames = frames;
    p->firstSample = frames > 0 ? interleaved[0] : 0.0f;
}

int32_t probeQueued(void* context) { return ((Probe*)context)->queued; }

// A one-frame looping tone at the output rate: every output frame reads sample
// index 0 and interpolates it with itself, so the mix is a constant the checks
// can name exactly without a bank.
audio::Sound toneSound() {
    audio::Sound s;
    // 0.5 in the int16 store: 16384 * (1/32768) is exactly 0.5f, so the mix this
    // tone produces is the same number it was when the store held a float (the
    // checks below name it). sound.h for why every value is exact.
    s.samples.push_back(16384);
    s.channels = 1;
    s.sampleRate = 44100;
    s.looping = true;
    return s;
}

audio::SoundBank::Entry toneEntry(const audio::Sound& sound) {
    audio::SoundBank::Entry e;
    e.volume = 0x7FFF;
    e.looping = true;
    e.channels = sound.channels;
    e.sampleRate = sound.sampleRate;
    e.frames = sound.frames();
    e.sound = &sound;
    return e;
}

// ---------------------------------------------------------------------------
// 1. The queue branch: `frames = 2646 - queued` (AudioDevice.cs:217-220 with
//    QueueSeconds 0.06f and bytesPerFrame 8).
// ---------------------------------------------------------------------------
void testQueueTopUp() {
    const audio::Sound sound = toneSound();
    const audio::SoundBank::Entry entry = toneEntry(sound);

    // The constant itself: (int)(44100 * 0.06f). 0.06f is 0.05999999865889549...
    // and 44100 * that is 2645.99994..., which rounds to the float 2646.0f, so
    // the C#'s cast is 2646 and not 2645.
    audio::SoundDevice device;
    expect("the C#'s (int)(OutputRate * QueueSeconds)", device.queueFrames(), 2646);

    memset(&g_probe, 0, sizeof(g_probe));
    device.setSink(probeSink, &g_probe, probeQueued);

    // An empty queue: the whole 60 ms is mixed.
    device.play(entry, 1.0f);
    g_probe.queued = 0;
    device.update(1.0f / 60.0f);
    expect("an empty queue tops up to the whole lookahead", device.mixedFrames(), 2646);
    expect("and the sink is called once", g_probe.calls, 1);

    // Half-drained: it tops up the difference, not another 60 ms.
    g_probe.calls = 0;
    g_probe.queued = 1000;
    device.update(1.0f / 60.0f);
    expect("a queue with 1000 frames left mixes the difference", device.mixedFrames(), 1646);
    expect("and the sink still gets one call", g_probe.calls, 1);

    // Exactly full: `frames <= 0` returns before the sink (AudioDevice.cs:225),
    // so nothing is mixed and nothing is queued.
    g_probe.calls = 0;
    g_probe.queued = 2646;
    device.update(1.0f / 60.0f);
    expect("a full queue mixes nothing", g_probe.calls, 0);

    // Over-full (the queue can exceed the lookahead when a frame runs long).
    g_probe.calls = 0;
    g_probe.queued = 5000;
    device.update(1.0f / 60.0f);
    expect("an over-full queue mixes nothing either", g_probe.calls, 0);

    // The mix that arrives is the mix the device made: a 0.5 tone at unity gain,
    // both channels, through the limiter's ceiling. The limiter only acts above
    // its ceiling, so a 0.5 tone passes untouched and the sink sees it.
    memset(&g_probe, 0, sizeof(g_probe));
    g_probe.queued = 0;
    device.update(1.0f / 60.0f);
    // A 0.5 sample at unity gain, unity bus and master, under the limiter's
    // ceiling: the sink sees the number the mix was made of, bit for bit.
    expect("the sink receives the device's own first sample", g_probe.firstSample == 0.5f ? 1 : 0,
           1);
}

// ---------------------------------------------------------------------------
// 2. The muted branch: `frames = (int)_owed; _owed -= frames` (the same lines,
//    with _stream == null). This is the branch golden/L3_audio_mix.txt pins and
//    the branch the ndsp sink keeps.
// ---------------------------------------------------------------------------
void testMutedAccounting() {
    const audio::Sound sound = toneSound();
    const audio::SoundBank::Entry entry = toneEntry(sound);

    audio::SoundDevice device;
    device.play(entry, 1.0f);
    // The delta arrives as a float, so 1/60 is 0.01666666753590107 and the
    // device's (double)44100 * (double)delta is 735.0000383332372 — the cast is
    // 735 either way, which is the C#'s own point ("a frame's 1/60 s is 735 of
    // them, not 734"). The digits here are the ones the code actually produces:
    // 44100 * (1.0/60.0) in pure double is exactly 735.0 and is NOT what this
    // branch computes (review F5 of run-20261106-035708).
    device.update(1.0f / 60.0f);
    expect("a muted frame mixes 735 frames", device.mixedFrames(), 735);

    // A frame too short to owe a whole frame owes nothing, and the fraction
    // carries: two 1/1000 s frames owe 44.1 and 88.2 -> 44 then 44 (88 - 44).
    audio::SoundDevice shorty;
    shorty.play(entry, 1.0f);
    shorty.update(1.0f / 1000.0f);
    expect("1 ms owes 44 frames", shorty.mixedFrames(), 44);
    shorty.update(1.0f / 1000.0f);
    expect("the next 1 ms owes 44 more, from the carried 0.1", shorty.mixedFrames(), 44);

    // A sink with no queue query keeps this branch: that is the ndsp sink's case,
    // and the reason it is a property of the seam and not of the target.
    audio::SoundDevice ndsp;
    memset(&g_probe, 0, sizeof(g_probe));
    ndsp.setSink(probeSink, &g_probe);  // no third argument: no queue query
    ndsp.play(entry, 1.0f);
    ndsp.update(1.0f / 60.0f);
    expect("a sink with no queue query keeps the owed accounting", ndsp.mixedFrames(), 735);
    expect("and the sink is still called", g_probe.calls, 1);
}

// ---------------------------------------------------------------------------
// 3. The queue branch resets _owed (`_owed -= _owed`, AudioDevice.cs:221), so a
//    device that had a queue and lost one does not then mix the whole backlog.
// ---------------------------------------------------------------------------
void testQueueResetsOwed() {
    const audio::Sound sound = toneSound();
    const audio::SoundBank::Entry entry = toneEntry(sound);

    audio::SoundDevice device;
    memset(&g_probe, 0, sizeof(g_probe));
    device.setSink(probeSink, &g_probe, probeQueued);
    device.play(entry, 1.0f);
    // Ten seconds through the queue branch: owed is reset every call, so the
    // backlog never grows past the lookahead.
    g_probe.queued = 0;
    for (int i = 0; i < 10; i++) {
        device.update(1.0f);
    }
    expect("a queued device never mixes more than the lookahead", device.mixedFrames(), 2646);

    // Now take the sink away: the next muted frame owes only its own delta, not
    // the ten seconds that passed.
    device.setSink(nullptr, nullptr, nullptr);
    device.update(1.0f / 60.0f);
    expect("after the queue goes away the owed is only the last delta", device.mixedFrames(),
           735);
}

// ---------------------------------------------------------------------------
// 4. The listener basis (Viewer.cs:686-693 -> FollowCamera.cs:201): the eye's
//    right is Cross(Forward, Up), which is also the right the view matrix is
//    built from (camera_builder.cpp's xaxis). A frontend that hands the mixer a
//    Camera must get that vector, not a mirrored one: a mirrored right mirrors
//    every pan in the game.
// ---------------------------------------------------------------------------
void testListenerBasis() {
    audio::SoundMixer mixer(nullptr);

    Camera camera;
    camera.position = Vec3{10.0f, 20.0f, 30.0f};
    camera.forward = Vec3{1.0f, 0.0f, 0.0f};
    camera.up = Vec3{0.0f, 0.0f, 1.0f};
    mixer.listenFromCamera(camera);
    expect("the listener stands where the camera does", mixer.listenerPosition.x == 10.0f &&
                                                                  mixer.listenerPosition.y == 20.0f &&
                                                                  mixer.listenerPosition.z == 30.0f
                                                              ? 1
                                                              : 0,
           1);
    // Cross(+X, +Z) = (0*1-0*0, 0*0-1*1, 1*0-0*0) = (0,-1,0).
    const bool rightOk = mixer.listenerRight.x == 0.0f && mixer.listenerRight.y == -1.0f &&
                         mixer.listenerRight.z == 0.0f;
    expect("the eye's right is Cross(Forward, Up)", rightOk ? 1 : 0, 1);
    expect("forward and up come from the camera too",
           mixer.listenerForward.x == 1.0f && mixer.listenerUp.z == 1.0f ? 1 : 0, 1);

    // A pitched view: forward 3-4-5 down, up the camera's own. The cross is
    // still the unit right, and it stays perpendicular to both.
    camera.forward = Vec3{0.8f, 0.0f, -0.6f};
    camera.up = Vec3{0.6f, 0.0f, 0.8f};
    mixer.listenFromCamera(camera);
    const float len = mixer.listenerRight.x * mixer.listenerRight.x +
                      mixer.listenerRight.y * mixer.listenerRight.y +
                      mixer.listenerRight.z * mixer.listenerRight.z;
    expect("a pitched view's right is unit", len > 0.9999f && len < 1.0001f ? 1 : 0, 1);
    expect("and it is (0,-1,0) for a view pitched in the XZ plane",
           mixer.listenerRight.y < -0.9999f ? 1 : 0, 1);
}

// ---------------------------------------------------------------------------
// 5. The int16 sample store is the oracle's float, bit for bit.
//
// The store holds the file's int16 and the scale is applied at the read
// (mdk_core/src/audio/sound.h). What makes that a store change and not a
// fidelity change is one arithmetic fact: BOTH of the oracle's scales are
// powers of two, so every value on both paths is exact. Every expected float
// below is written the ORACLE's way — `(byte - 128) * (1f/128f)` and
// `short * (1f/32768f)`, the ByteScale/ShortScale multiplies of
// AudioDevice.cs — and compared as bits, never read back from the port. A store
// that shifted by 7 instead of 8, a read that divided instead of multiplying, or
// a bias applied after the shift is red here, at the value, rather than as an
// unexplained 1-ULP drift in a golden digest.
//
// The extremes are the ones that would break a non-exact scheme: byte 0 and
// short -32768 are the two values that reach -1.0 exactly, and byte 255 /
// short 32767 are the two that do NOT reach +1.0.
// ---------------------------------------------------------------------------
void testSampleStoreExact() {
    // The store's width is the batch's claim: level 3's 3,234,976 decoded samples
    // are 6,469,952 bytes here and were 12,939,904 as float. What it is measured
    // AGAINST is the 24 MiB the build's crt0 header asks for (`__heap_size =
    // 0x01800000`); the "16 MB of FCRAM" this comment used to cite is a figure
    // neither this console nor this build has, and sound.h carries the three
    // numbers that are sourced. Pinned so widening the store is a red test and not
    // a quiet regression; the same fact is a static_assert in sound.h.
    expect("the sample store is two bytes per sample",
           (long long)sizeof(audio::Sound::Samples::value_type), 2);

    // --- 8-bit unsigned, biased at 128 ---
    const uint8_t bytes8[] = {0, 127, 128, 129, 200, 255};
    audio::Sound eight;
    audio::soundFromPcm(bytes8, 6, 1, 11025, 8, false, &eight);
    expect("every 8-bit byte decoded", (long long)eight.samples.size(), 6);
    const float kByteScale = 1.0f / 128.0f;  // AudioDevice.cs ByteScale
    for (int i = 0; i < 6; i++) {
        const float want = (float)((int32_t)bytes8[i] - 128) * kByteScale;
        expectBits("the 8-bit decode is the oracle's (byte-128)*(1/128)", eight.sampleAt(i), want);
    }
    expectBits("byte 0 is exactly -1.0", eight.sampleAt(0), -1.0f);
    expectBits("byte 128 is exactly 0", eight.sampleAt(2), 0.0f);
    expectBits("byte 255 is 127/128 and not 1.0", eight.sampleAt(5), 0.9921875f);

    // --- 16-bit signed little-endian ---
    const int16_t shorts[] = {0, 1, -1, 16384, 32767, -32768};
    uint8_t bytes16[sizeof(shorts)];
    for (int i = 0; i < 6; i++) {
        bytes16[(size_t)i * 2] = (uint8_t)((uint16_t)shorts[i] & 0xFF);
        bytes16[(size_t)i * 2 + 1] = (uint8_t)((uint16_t)shorts[i] >> 8);
    }
    audio::Sound sixteen;
    audio::soundFromPcm(bytes16, (int32_t)sizeof(bytes16), 1, 22050, 16, false, &sixteen);
    expect("every 16-bit short decoded", (long long)sixteen.samples.size(), 6);
    const float kShortScale = 1.0f / 32768.0f;  // AudioDevice.cs ShortScale
    for (int i = 0; i < 6; i++) {
        const float want = (float)shorts[i] * kShortScale;
        expectBits("the 16-bit decode is the oracle's short*(1/32768)", sixteen.sampleAt(i), want);
    }
    expectBits("short -32768 is exactly -1.0", sixteen.sampleAt(5), -1.0f);
    expectBits("short 16384 is exactly 0.5", sixteen.sampleAt(3), 0.5f);
    expectBits("short 32767 is 32767/32768 and not 1.0", sixteen.sampleAt(4), 0.99996948f);

    // The 16-bit path drops a trailing odd byte, as `data.Length & ~1` does, and
    // frames() divides the samples by the channel count as Sound.Frames does.
    audio::Sound odd;
    audio::soundFromPcm(bytes16, (int32_t)sizeof(bytes16) - 1, 2, 22050, 16, false, &odd);
    expect("an odd trailing byte is dropped", (long long)odd.samples.size(), 5);
    expect("and frames() divides the samples by the channels", odd.frames(), 2);

    // A bank that has decoded nothing reports no memory, which is what makes
    // decodedBytes() a measurement and not a constant.
    audio::SoundBank empty;
    expect("an undecoded bank holds no samples", (long long)empty.decodedSamples(), 0);
    expect("and no bytes", (long long)empty.decodedBytes(), 0);
    expect("and no archives", (long long)empty.archiveBytes(), 0);
}

// ---------------------------------------------------------------------------
// 6. The stereo interleave, executed.
//
// Every sound in MDK's own archives is mono (sound.h), so the `channels != 1`
// branch of SoundDevice::sample() was reached by no game data and by no test —
// which is exactly how a signed `frame * 2` got written into it and nobody
// noticed (review-1 F4 of run-20261108-195620). The pair is now formed in
// size_t, where the multiply cannot overflow, and this is the case that runs the
// branch at all: a one-frame looping STEREO tone whose two channels are named
// and opposite, so a swapped interleave, a pair built from the wrong index, or a
// stereo sound read as mono each move a bit rather than hiding in a branch no
// test reaches.
// ---------------------------------------------------------------------------
void testStereoInterleave() {
    audio::Sound stereo;
    stereo.samples.push_back(16384);   // left  = +(16384/32768) = +0.5 exactly
    stereo.samples.push_back(-16384);  // right = -0.5 exactly
    stereo.channels = 2;
    stereo.sampleRate = audio::SoundDevice::kOutputRate;
    stereo.looping = true;

    audio::SoundBank::Entry entry;
    entry.volume = 0x7FFF;
    entry.looping = true;
    entry.channels = stereo.channels;
    entry.sampleRate = stereo.sampleRate;
    entry.frames = stereo.frames();
    entry.sound = &stereo;

    expect("a one-frame stereo sound has one frame", stereo.frames(), 1);
    expect("and its frames() divided the samples by the channels", (long long)stereo.samples.size(),
           2);

    audio::SoundDevice device;
    device.play(entry, 1.0f);
    device.update(1.0f / 60.0f);
    expect("the muted device mixes a frame's worth of stereo", device.mixedFrames(), 735);
    const float* mix = device.mix();
    expect("and the mix is there", mix != nullptr ? 1 : 0, 1);
    // The interleave is even = left, odd = right, which is what the ndsp sink's
    // PCM16 stereo slot and the SDL sink's F32LE buffer both require.
    expectBits("the LEFT output is the even sample", mix[0], 0.5f);
    expectBits("and the RIGHT output is the odd one, with its own sign", mix[1], -0.5f);
}

} // namespace

namespace mdk {
namespace test {

int runAudioSinkTests() {
    failures = 0;
    testQueueTopUp();
    testMutedAccounting();
    testQueueResetsOwed();
    testListenerBasis();
    testSampleStoreExact();
    testStereoInterleave();
    return failures;
}

} // namespace test
} // namespace mdk
