// ============================================================================
// voice_binding_tests.cpp -- see voice_binding_tests.h.
// ============================================================================

#include "voice_binding_tests.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "audio/sound_device.h"
#include "audio/voice_binding.h"
#include "audio/voice_lifecycle.h"

using namespace mdk;

namespace {

int failures = 0;

void check(const char* what, bool ok) {
    if (!ok) {
        printf("  FAIL: %s\n", what);
        failures++;
    }
}

void expectInt(const char* what, long long got, long long want) {
    if (got != want) {
        printf("  FAIL: %s was %lld, expected %lld\n", what, got, want);
        failures++;
    }
}

bool sameBits(double a, double b) {
    uint64_t x = 0, y = 0;
    memcpy(&x, &a, sizeof(x));
    memcpy(&y, &b, sizeof(y));
    return x == y;
}

bool sameBits(float a, float b) {
    uint32_t x = 0, y = 0;
    memcpy(&x, &a, sizeof(x));
    memcpy(&y, &b, sizeof(y));
    return x == y;
}

// A deterministic generator, so a failure reproduces (xorshift32).
struct Rng {
    uint32_t s;
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    // [0, n)
    int32_t below(int32_t n) { return (int32_t)(next() % (uint32_t)n); }
    // A float in [lo, hi), from 24 random bits.
    float range(float lo, float hi) {
        return lo + (hi - lo) * ((float)(next() >> 8) * (1.0f / 16777216.0f));
    }
};

// ---------------------------------------------------------------------------
// 1a. lifecycleAdvance == lifecycleStep, bit for bit.
// ---------------------------------------------------------------------------

// One case: run both from the same position; FALSE on any difference.
bool sameAdvance(double position, float step, int32_t frames, bool looping, int32_t count,
                 bool* solved) {
    double a = position;
    double b = position;
    const bool liveA = audio::lifecycleStep(&a, step, frames, looping, count);
    const bool liveB = audio::lifecycleAdvance(&b, step, frames, looping, count, solved);
    if (liveA != liveB || !sameBits(a, b)) {
        printf("    position %.17g step %.9g (%08X) frames %d looping %d count %d: "
               "loop (%d, %.17g) solved (%d, %.17g)\n",
               position, (double)step,
               [&] { uint32_t u = 0; memcpy(&u, &step, 4); return u; }(), (int)frames,
               (int)looping, (int)count, (int)liveA, a, (int)liveB, b);
        return false;
    }
    return true;
}

void testLifecycleSolvedEqualsStepped() {
    Rng rng = {0x9E3779B9u};
    const int32_t rates[] = {8000, 11025, 11111, 14000, 16000, 20000, 22050, 32000, 44100};
    const int32_t lengths[] = {1, 2, 3, 7, 100, 735, 736, 4410, 22050, 123457, 600000};
    int32_t mismatches = 0;
    int32_t solvedCount = 0;
    int32_t total = 0;

    // Whole voice lives: the device's own schedule (owed frames per update, a
    // Doppler pitch that changes every update), until the voice ends or 400
    // updates pass.
    for (int32_t life = 0; life < 3000; life++) {
        const int32_t rate = rates[rng.below((int32_t)(sizeof(rates) / sizeof(rates[0])))];
        const int32_t frames = lengths[rng.below((int32_t)(sizeof(lengths) / sizeof(lengths[0])))];
        const bool looping = rng.below(3) == 0;
        double owed = 0.0;
        double position = 0.0;
        bool alive = true;
        for (int32_t u = 0; u < 400 && alive; u++) {
            const float delta = rng.below(10) == 0 ? rng.range(0.001f, 0.25f)
                                                   : rng.range(1.0f / 70.0f, 1.0f / 25.0f);
            owed += 44100.0 * (double)delta;
            const int32_t count = (int32_t)owed;
            owed -= (double)count;
            const float pitch = rng.below(4) == 0 ? 1.0f : rng.range(0.25f, 3.0f);
            const float step = pitch * (float)rate / 44100.0f;  // the device's expression
            double a = position;
            double b = position;
            bool solved = false;
            const bool liveA = audio::lifecycleStep(&a, step, frames, looping, count);
            const bool liveB =
                audio::lifecycleAdvance(&b, step, frames, looping, count, &solved);
            total++;
            solvedCount += solved ? 1 : 0;
            if (liveA != liveB || !sameBits(a, b)) {
                if (mismatches < 5) {
                    printf("    life %d update %d: frames %d looping %d step %.9g count %d: "
                           "loop (%d, %.17g) solved (%d, %.17g)\n",
                           (int)life, (int)u, (int)frames, (int)looping, (double)step,
                           (int)count, (int)liveA, a, (int)liveB, b);
                }
                mismatches++;
            }
            position = a;
            alive = liveA;
        }
    }
    expectInt("solved lifecycle updates that differ from the stepped loop", mismatches, 0);
    // The point of solving it: almost every update must take the solved path, or the
    // ARM11 is still running the per-sample loop under another name.
    check("the solved path covers at least 99% of the device's own updates",
          (long long)solvedCount * 100 >= (long long)total * 99);
    printf("    lifecycle: %d of %d updates solved\n", (int)solvedCount, (int)total);

    // The corners, each against the loop.
    bool solved = false;
    check("a one-frame loop, many wraps", sameAdvance(0.0, 0.37f, 1, true, 735, &solved));
    check("a two-frame loop stepping 3 (step > frames)", sameAdvance(0.0, 3.0f, 2, true, 735, &solved));
    check("a one-shot whose step passes its end in one frame",
          sameAdvance(0.0, 5.0f, 3, false, 735, &solved));
    check("a looping position far past the end (2n + a bit)",
          sameAdvance(9.25, 0.5f, 4, true, 100, &solved));
    check("a one-shot already past its end ends at once",
          sameAdvance(10.0, 1.0f, 10, false, 735, &solved));
    check("a one-shot that lands exactly on its end",
          sameAdvance(0.0, 0.5f, 100, false, 200, &solved));
    check("a one-shot that lands exactly on its end one frame later",
          sameAdvance(0.0, 0.5f, 100, false, 201, &solved));
    check("exact landings on a wrap", sameAdvance(0.0, 0.25f, 10, true, 1000, &solved));
    check("a subnormal step (the loop's, not the proof's)",
          sameAdvance(0.0, 1e-40f, 10, false, 735, &solved));
    check("a step below the proof's quantum falls back", sameAdvance(1.0, 1e-30f, 10, true, 735, &solved));
    check("a zero step never ends", sameAdvance(3.0, 0.0f, 10, false, 735, &solved));
    check("a negative step (setPitch is not clamped)", sameAdvance(3.0, -0.5f, 10, false, 735, &solved));
    check("a position with a low bit far below the step's",
          sameAdvance(1.0 + 1.0 / 1099511627776.0, 0.75f, 1000, true, 735, &solved));
    check("a huge sound", sameAdvance(0.0, 2.9f, 0x7FFFFFFF, false, 11025, &solved));
    check("a huge position (past the 2^53 quantum bound)",
          sameAdvance(9007199254740991.0, 0.5f, 100, true, 10, &solved));
    check("no frames to play: frames == 0 looping", sameAdvance(0.0, 0.5f, 0, true, 735, &solved));
    check("no frames to play: frames == 0 one-shot", sameAdvance(0.0, 0.5f, 0, false, 735, &solved));
    check("count 0 changes nothing", sameAdvance(1.5, 0.5f, 10, false, 0, &solved));
    // A random sweep of the corners themselves.
    int32_t bad = 0;
    for (int32_t i = 0; i < 200000; i++) {
        const int32_t frames = 1 + rng.below(rng.below(2) ? 8 : 5000);
        const float step = rng.below(5) == 0 ? (float)(1 + rng.below(9)) * 0.125f
                                             : rng.range(0.0001f, 9.0f);
        const double position = rng.below(3) == 0 ? (double)rng.below(frames * 3 + 1) * 0.5
                                                  : (double)rng.range(0.0f, (float)frames * 1.5f);
        const int32_t count = 1 + rng.below(rng.below(4) == 0 ? 11025 : 800);
        if (!sameAdvance(position, step, frames, rng.below(2) == 0, count, &solved)) {
            bad++;
            if (bad > 3) {
                break;
            }
        }
    }
    expectInt("random corner cases that differ from the loop", bad, 0);
}

// ---------------------------------------------------------------------------
// 1b. A device with the sample plane OFF against a twin with it ON.
// ---------------------------------------------------------------------------

struct Bank {
    std::vector<audio::Sound> sounds;  // filled once; addresses stable after
    std::vector<audio::SoundBank::Entry> entries;
};

void makeBank(Bank* bank) {
    const int32_t lengths[] = {1, 3, 300, 735, 2000, 11025, 30000};
    const int32_t rates[] = {11025, 22050, 44100, 8000};
    bank->sounds.reserve(64);
    for (int32_t i = 0; i < 7; i++) {
        audio::Sound s;
        s.channels = 1;
        s.sampleRate = rates[i % 4];
        s.looping = (i % 3) == 1;
        s.samples.resize((size_t)lengths[i]);
        for (int32_t k = 0; k < lengths[i]; k++) {
            s.samples[(size_t)k] = (int16_t)((k * 977) % 65536 - 32768);
        }
        bank->sounds.push_back(s);
    }
    for (size_t i = 0; i < bank->sounds.size(); i++) {
        audio::SoundBank::Entry e;
        e.volume = 0x7FFF;
        e.looping = bank->sounds[i].looping;
        e.channels = 1;
        e.sampleRate = bank->sounds[i].sampleRate;
        e.frames = bank->sounds[i].frames();
        e.sound = &bank->sounds[i];
        bank->entries.push_back(e);
    }
    // And an entry with nothing to play: its lifecycle must still run.
    audio::SoundBank::Entry none;
    none.volume = 0x4000;
    none.sampleRate = 22050;
    none.frames = 5000;
    none.looping = false;
    bank->entries.push_back(none);
}

bool sameVoices(const audio::SoundDevice& a, const audio::SoundDevice& b) {
    const std::vector<audio::SoundDevice::Voice>& va = a.voices();
    const std::vector<audio::SoundDevice::Voice>& vb = b.voices();
    if (va.size() != vb.size()) {
        return false;
    }
    for (size_t i = 0; i < va.size(); i++) {
        if (va[i].id != vb[i].id || !sameBits(va[i].position, vb[i].position)) {
            return false;
        }
    }
    return true;
}

void testDeviceWithoutTheMixKeepsTheLifecycle() {
    Bank bank;
    makeBank(&bank);
    audio::SoundDevice mixed;
    audio::SoundDevice silent;
    silent.setSampleMix(false);
    Rng rng = {12345u};
    int32_t diverged = -1;
    int32_t ended = 0;
    for (int32_t u = 0; u < 6000 && diverged < 0; u++) {
        // Plays (sometimes a burst past the 64-voice cap), stops and Doppler.
        const int32_t plays = rng.below(10) == 0 ? 12 : rng.below(3);
        for (int32_t p = 0; p < plays; p++) {
            const audio::SoundBank::Entry& e =
                bank.entries[(size_t)rng.below((int32_t)bank.entries.size())];
            const int32_t bus = rng.below(8) == 0 ? audio::SoundDevice::Music
                                                  : audio::SoundDevice::Effects;
            const int32_t ida = mixed.play(e, 0.5f, 1.0f, 0.0f, bus);
            const int32_t idb = silent.play(e, 0.5f, 1.0f, 0.0f, bus);
            if (ida != idb) {
                diverged = u;
            }
        }
        const std::vector<audio::SoundDevice::Voice>& live = mixed.voices();
        if (!live.empty() && rng.below(6) == 0) {
            const int32_t id = live[(size_t)rng.below((int32_t)live.size())].id;
            mixed.stop(id);
            silent.stop(id);
        }
        for (size_t i = 0; i < mixed.voices().size(); i++) {
            if (rng.below(2) == 0) {
                const int32_t id = mixed.voices()[i].id;
                const float pitch = rng.range(0.25f, 3.0f);
                const float pan = rng.range(-1.0f, 1.0f);
                mixed.set(id, 0.7f, pitch, pan);
                silent.set(id, 0.7f, pitch, pan);
            }
        }
        std::vector<int32_t> before;
        for (size_t i = 0; i < mixed.voices().size(); i++) {
            before.push_back(mixed.voices()[i].id);
        }
        const float delta = rng.below(20) == 0 ? 0.25f : rng.range(1.0f / 70.0f, 1.0f / 25.0f);
        mixed.update(delta);
        silent.update(delta);
        if (!sameVoices(mixed, silent) || mixed.mixedFrames() != silent.mixedFrames()) {
            diverged = u;
        }
        // The ended sets agree too (the voices a hardware channel lets finish).
        for (size_t i = 0; i < before.size(); i++) {
            const bool em = mixed.endedInLastUpdate(before[i]);
            if (em != silent.endedInLastUpdate(before[i])) {
                diverged = u;
            }
            ended += em ? 1 : 0;
        }
    }
    check("the schedule ends voices naturally (the comparison is not vacuous)", ended > 100);
    expectInt("first update where the mix-off device's voices differ from the mixed twin's "
              "(-1: never)",
              diverged, -1);
    check("the mix-off device hands out no buffer", silent.mix() == nullptr);
    check("the mixed twin still does", mixed.mix() != nullptr);
}

// The ended set: a one-shot that runs out is ENDED; one that is stopped is not.
void testEndedIsNotStopped() {
    Bank bank;
    makeBank(&bank);
    audio::SoundDevice d;
    d.setSampleMix(false);
    const audio::SoundBank::Entry& shortOne = bank.entries[0];  // 1 frame at 11025, one-shot
    const audio::SoundBank::Entry& longOne = bank.entries[6];   // 30000 frames
    const int32_t a = d.play(shortOne, 1.0f);
    const int32_t b = d.play(longOne, 1.0f);
    d.stop(b);
    d.update(1.0f / 60.0f);
    check("a 1-frame one-shot ends in its first update", !d.isPlaying(a));
    check("...and is ENDED", d.endedInLastUpdate(a));
    check("a stopped voice is not ENDED", !d.endedInLastUpdate(b));
    d.update(1.0f / 60.0f);
    check("ENDED lasts one update only", !d.endedInLastUpdate(a));
}

// ---------------------------------------------------------------------------
// 2. The binder.
// ---------------------------------------------------------------------------

audio::SoundBank::Entry entryOf(const audio::Sound& s, bool looping) {
    audio::SoundBank::Entry e;
    e.volume = 0x7FFF;
    e.looping = looping;
    e.channels = s.channels;
    e.sampleRate = s.sampleRate;
    e.frames = s.frames();
    e.sound = &s;
    return e;
}

audio::Sound longSound() {
    audio::Sound s;
    s.channels = 1;
    s.sampleRate = 22050;
    s.samples.assign(22050 * 4, (int16_t)256);  // four seconds
    return s;
}

audio::Sound shortSound(int32_t frames) {
    audio::Sound s;
    s.channels = 1;
    s.sampleRate = 22050;
    s.samples.assign((size_t)frames, (int16_t)256);
    return s;
}

void testAllocation() {
    const audio::Sound snd = longSound();
    audio::SoundDevice d;
    d.setSampleMix(false);
    audio::VoiceBinder binder;
    const int32_t a = d.play(entryOf(snd, false), 1.0f);
    const int32_t b = d.play(entryOf(snd, false), 1.0f);
    const int32_t c = d.play(entryOf(snd, false), 1.0f);
    binder.sync(d, nullptr);
    expectInt("first voice on channel 0", binder.channelOf(a), 0);
    expectInt("second voice on channel 1", binder.channelOf(b), 1);
    expectInt("third voice on channel 2", binder.channelOf(c), 2);
    check("a new voice is started", binder.channel(0).start);
    expectInt("from its first frame (seen before its first update)", binder.channel(0).startFrame, 0);
    check("with its samples", binder.channel(0).sound == &snd);
    check("not as a rebind", !binder.channel(0).rebind);
    d.update(1.0f / 60.0f);
    binder.sync(d, nullptr);
    check("a voice already started is not started again", !binder.channel(0).start);
    expectInt("three bound", binder.boundCount(), 3);
}

void testEvictionNeverEndsALogicalVoice() {
    const audio::Sound snd = shortSound(22050 / 4);  // a quarter second
    audio::SoundDevice d;
    audio::SoundDevice twin;  // the same schedule, and no binder ever reads it
    d.setSampleMix(false);
    twin.setSampleMix(false);
    audio::VoiceBinder binder;
    int32_t ids[9];
    for (int32_t i = 0; i < 8; i++) {
        ids[i] = d.play(entryOf(snd, false), 1.0f);
        twin.play(entryOf(snd, false), 1.0f);
        binder.sync(d, nullptr);
        d.update(1.0f / 60.0f);
        twin.update(1.0f / 60.0f);
    }
    ids[8] = d.play(entryOf(snd, false), 1.0f);
    twin.play(entryOf(snd, false), 1.0f);
    binder.sync(d, nullptr);
    expectInt("the ninth voice takes channel 0, the oldest claim", binder.channelOf(ids[8]), 0);
    expectInt("the first voice has lost its channel", binder.channelOf(ids[0]), -1);
    check("an evicted one-shot is not waiting to come back", !binder.waiting(ids[0]));
    check("...but it is still PLAYING in the game's eyes", d.isPlaying(ids[0]));
    expectInt("one eviction counted", binder.stats().evictions, 1);
    // Run both devices to the end of every voice: each voice ends on the same
    // update on both, binder or not.
    int32_t firstDiff = -1;
    for (int32_t u = 0; u < 60; u++) {
        d.update(1.0f / 60.0f);
        twin.update(1.0f / 60.0f);
        binder.sync(d, nullptr);
        for (int32_t i = 0; i < 9; i++) {
            if (d.isPlaying(ids[i]) != twin.isPlaying(ids[i])) {
                firstDiff = u;
            }
        }
    }
    expectInt("first update where a bound device and an unbound twin disagree (-1: never)",
              firstDiff, -1);
    check("every voice has ended by then", d.voiceCount() == 0);
    expectInt("and every channel is free", binder.boundCount(), 0);
}

void testReleaseOnEnd() {
    const audio::Sound tiny = shortSound(100);
    const audio::Sound snd = longSound();
    audio::SoundDevice d;
    d.setSampleMix(false);
    audio::VoiceBinder binder;
    const int32_t a = d.play(entryOf(tiny, false), 1.0f);
    const int32_t b = d.play(entryOf(snd, false), 1.0f);
    binder.sync(d, nullptr);
    expectInt("the short one on channel 0", binder.channelOf(a), 0);
    d.update(1.0f / 60.0f);  // 100 frames at 22050 run out within one 1/60 s update
    check("the short one has ended", !d.isPlaying(a));
    d.stop(b);
    bool sounding[audio::VoiceBinder::kChannels] = {};
    sounding[0] = true;  // the DSP is still finishing it
    sounding[1] = true;
    binder.sync(d, sounding);
    check("an ENDED voice is not cut: the DSP finishes its last frame", !binder.channel(0).cut);
    check("...its channel is draining", binder.channel(0).draining);
    expectInt("...and no longer bound", binder.channel(0).voice, 0);
    check("a STOPPED voice is cut now", binder.channel(1).cut);
    check("...and its channel is idle", !binder.channel(1).draining && binder.channel(1).voice == 0);
    // A new voice prefers the idle channel to the draining one.
    const int32_t c = d.play(entryOf(snd, false), 1.0f);
    binder.sync(d, sounding);
    expectInt("a new voice takes an idle channel before a draining one", binder.channelOf(c), 1);
    sounding[0] = false;
    binder.sync(d, sounding);
    check("a draining channel that went quiet is idle", !binder.channel(0).draining);
    expectInt("drains counted", binder.stats().drains, 1);
    expectInt("cuts counted", binder.stats().cuts, 1);
}

void testLoopingVoiceComesBack() {
    const audio::Sound loop = longSound();
    const audio::Sound shot = shortSound(22050 / 10);
    audio::SoundDevice d;
    d.setSampleMix(false);
    audio::VoiceBinder binder;
    const int32_t hum = d.play(entryOf(loop, true), 1.0f);
    binder.sync(d, nullptr);
    for (int32_t u = 0; u < 30; u++) {  // half a second in
        d.update(1.0f / 60.0f);
        binder.sync(d, nullptr);
    }
    int32_t shots[8];
    for (int32_t i = 0; i < 8; i++) {
        shots[i] = d.play(entryOf(shot, false), 1.0f);
    }
    binder.sync(d, nullptr);
    expectInt("a burst of eight evicts the ambience", binder.channelOf(hum), -1);
    check("the evicted LOOP waits for a channel", binder.waiting(hum));
    check("and plays on in the game's eyes", d.isPlaying(hum));
    bool sounding[audio::VoiceBinder::kChannels];
    for (int32_t c = 0; c < audio::VoiceBinder::kChannels; c++) {
        sounding[c] = true;
    }
    // The shots run out; while the DSP is still finishing them nothing comes back.
    for (int32_t u = 0; u < 8; u++) {
        d.update(1.0f / 60.0f);
        binder.sync(d, sounding);  // once a frame, as the frontend does
    }
    expectInt("no channel is idle while the shots drain", binder.channelOf(hum), -1);
    for (int32_t c = 0; c < audio::VoiceBinder::kChannels; c++) {
        sounding[c] = false;
    }
    const audio::SoundDevice::Voice* v = nullptr;
    for (size_t i = 0; i < d.voices().size(); i++) {
        if (d.voices()[i].id == hum) {
            v = &d.voices()[i];
        }
    }
    binder.sync(d, sounding);
    const int32_t c = binder.channelOf(hum);
    check("the loop is rebound the first frame a channel is idle", c >= 0);
    if (c >= 0 && v != nullptr) {
        check("as a rebind", binder.channel(c).rebind && binder.channel(c).start);
        expectInt("at its logical position", binder.channel(c).startFrame, (long long)(int32_t)v->position);
        check("which is not the start", binder.channel(c).startFrame > 0);
        check("looping", binder.channel(c).looping);
    }
    check("and no longer waits", !binder.waiting(hum));
    expectInt("one rebind counted", binder.stats().rebinds, 1);
    (void)shots;
}

void testMusicPool() {
    const audio::Sound snd = longSound();
    audio::SoundDevice d;
    d.setSampleMix(false);
    audio::VoiceBinder binder;
    for (int32_t i = 0; i < 8; i++) {
        d.play(entryOf(snd, false), 1.0f);
    }
    const int32_t m1 = d.play(entryOf(snd, true), 1.0f, 1.0f, 0.0f, audio::SoundDevice::Music);
    binder.sync(d, nullptr);
    expectInt("music takes the first music channel even with the effects pool full",
              binder.channelOf(m1), audio::VoiceBinder::kFirstMusicChannel);
    const int32_t e9 = d.play(entryOf(snd, false), 1.0f);
    binder.sync(d, nullptr);
    check("an effect never takes a music channel", binder.channelOf(e9) < audio::VoiceBinder::kEffectChannels);
    check("...nor evicts the music", binder.channelOf(m1) == audio::VoiceBinder::kFirstMusicChannel);
    const int32_t m2 = d.play(entryOf(snd, true), 1.0f, 1.0f, 0.0f, audio::SoundDevice::Music);
    binder.sync(d, nullptr);
    expectInt("the crossfade's second track takes the second music channel", binder.channelOf(m2),
              audio::VoiceBinder::kFirstMusicChannel + 1);
    const int32_t m3 = d.play(entryOf(snd, true), 1.0f, 1.0f, 0.0f, audio::SoundDevice::Music);
    binder.sync(d, nullptr);
    expectInt("a third track evicts the oldest music claim", binder.channelOf(m3),
              audio::VoiceBinder::kFirstMusicChannel);
}

void testMixFromTheLaws() {
    const audio::Sound snd = longSound();
    audio::SoundDevice d;
    d.setSampleMix(false);
    d.setBusGain(audio::SoundDevice::Effects, 0.5f);
    audio::VoiceBinder binder;
    const int32_t a = d.play(entryOf(snd, false), 0.8f, 1.0f, 0.0f);
    d.set(a, 0.8f, 1.25f, 0.5f);
    binder.sync(d, nullptr);
    const audio::VoiceBinder::Channel& ch = binder.channel(binder.channelOf(a));
    float left = 0.0f, right = 0.0f;
    audio::SoundDevice::panGains(0.8f * 0.5f * 1.0f, 0.5f, &left, &right);
    check("left is the pan law over gain x bus x master", sameBits(ch.left, left));
    check("right likewise", sameBits(ch.right, right));
    check("panned right: the left channel is the far one", ch.left < ch.right);
    check("rate is sampleRate x pitch", sameBits(ch.rate, 22050.0f * 1.25f));
    check("a fresh channel's mix is new", ch.mixChanged);
    binder.sync(d, nullptr);
    check("an unchanged mix is not re-sent", !binder.channel(binder.channelOf(a)).mixChanged);
    d.set(a, 0.4f, 1.25f, 0.5f);
    binder.sync(d, nullptr);
    check("a changed gain is", binder.channel(binder.channelOf(a)).mixChanged);
    // The pan law itself: the oracle's MixVoice numbers (AudioDevice.PanRangeDb).
    audio::SoundDevice::panGains(1.0f, 0.0f, &left, &right);
    check("centre: both at gain", left == 1.0f && right == 1.0f);
    audio::SoundDevice::panGains(1.0f, -1.0f, &left, &right);
    check("hard left: right at -100 dB", left == 1.0f && sameBits(right, powf(10.0f, -100.0f / 20.0f)));
}

void testSilentAndReplacedDevice() {
    const audio::Sound snd = longSound();
    audio::SoundDevice* d = new audio::SoundDevice();
    d->setSampleMix(false);
    audio::VoiceBinder binder;
    audio::SoundBank::Entry none;
    none.frames = 1000;
    none.sampleRate = 22050;
    const int32_t n = d->play(none, 1.0f);
    const int32_t a = d->play(entryOf(snd, true), 1.0f);
    const int32_t b = d->play(entryOf(snd, true), 1.0f);
    binder.sync(*d, nullptr);
    expectInt("a voice with nothing to play gets no channel", binder.channelOf(n), -1);
    expectInt("...and the next one takes channel 0", binder.channelOf(a), 0);
    expectInt("silent voices counted", binder.stats().silent, 1);
    (void)b;
    delete d;
    // The runtime is rebuilt: a new device, ids from 1 again.
    audio::SoundDevice fresh;
    fresh.setSampleMix(false);
    const int32_t f = fresh.play(entryOf(snd, false), 1.0f);
    binder.sync(fresh, nullptr);
    check("a device whose ids ran backwards cuts the old channels", binder.channel(1).cut);
    expectInt("and its first voice is bound", binder.channelOf(f), 0);
    check("on a fresh start", binder.channel(0).start && !binder.channel(0).rebind);
    // An explicit reset (the backend's detach).
    binder.reset();
    check("reset cuts what was bound", binder.channel(0).cut);
    expectInt("and nothing is bound after it", binder.boundCount(), 0);
}

// A reset that leaves the device alive (a respawn with no script assets keeps
// the standalone mixer): what is under way is adopted, not replayed.
void testResetAdoptsTheLiveDevice() {
    const audio::Sound snd = longSound();
    audio::SoundDevice d;
    d.setSampleMix(false);
    audio::VoiceBinder binder;
    const int32_t loop = d.play(entryOf(snd, true), 1.0f);
    const int32_t shot = d.play(entryOf(snd, false), 1.0f);
    binder.sync(d, nullptr);
    for (int32_t u = 0; u < 10; u++) {
        d.update(1.0f / 60.0f);
        binder.sync(d, nullptr);
    }
    binder.reset();
    const int32_t fresh = d.play(entryOf(snd, false), 1.0f);  // not yet advanced
    binder.sync(d, nullptr);
    check("a one-shot under way at a reset is not replayed", binder.channelOf(shot) < 0);
    const int32_t c = binder.channelOf(loop);
    check("a loop under way comes back", c >= 0);
    if (c >= 0) {
        check("...as a rebind at its position, not from the start",
              binder.channel(c).rebind && binder.channel(c).startFrame > 0);
    }
    check("a voice that has not sounded yet is bound as new",
          binder.channelOf(fresh) >= 0 && !binder.channel(binder.channelOf(fresh)).rebind &&
              binder.channel(binder.channelOf(fresh)).startFrame == 0);
    expectInt("two adopted", binder.stats().adopted, 2);
    // A second, different device is noticed without an explicit reset.
    audio::SoundDevice other;
    other.setSampleMix(false);
    for (int32_t i = 0; i < 5; i++) {
        other.play(entryOf(snd, false), 1.0f);  // ids 1-5: ABOVE the old lastSeen (3)
    }
    binder.sync(other, nullptr);
    expectInt("a different device: its unsounded voices are bound, the old ones are cut",
              binder.boundCount(), 5);
}

} // namespace

namespace mdk {
namespace test {

int runVoiceBindingTests() {
    failures = 0;
    testLifecycleSolvedEqualsStepped();
    testDeviceWithoutTheMixKeepsTheLifecycle();
    testEndedIsNotStopped();
    testAllocation();
    testEvictionNeverEndsALogicalVoice();
    testReleaseOnEnd();
    testLoopingVoiceComesBack();
    testMusicPool();
    testMixFromTheLaws();
    testSilentAndReplacedDevice();
    testResetAdoptsTheLiveDevice();
    return failures;
}

} // namespace test
} // namespace mdk
