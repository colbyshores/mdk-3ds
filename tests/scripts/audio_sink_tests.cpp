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
    s.samples.push_back(0.5f);
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
    // 44100 * (1/60) is 735.00000000000007 in double, so the cast is 735 — the
    // C#'s own comment on _owed says exactly this ("a frame's 1/60 s is 735 of
    // them, not 734").
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

} // namespace

namespace mdk {
namespace test {

int runAudioSinkTests() {
    failures = 0;
    testQueueTopUp();
    testMutedAccounting();
    testQueueResetsOwed();
    testListenerBasis();
    return failures;
}

} // namespace test
} // namespace mdk
