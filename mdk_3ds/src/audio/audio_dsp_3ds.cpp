// ============================================================================
// audio_dsp_3ds.cpp -- the DSP-mixed output (see audio_dsp_3ds.h).
// ============================================================================

#include "audio/audio_dsp_3ds.h"

#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>

#include <string>

#include "audio/dsp_adpcm.h"
#include "audio/voice_binding.h"
#include "platform/log_3ds.h"
#include "platform/stream_worker_3ds.h"
#include "platform/vfp_fpscr.h"

namespace mdk {
namespace {

using audio::VoiceBinder;

// ---- the channels ----------------------------------------------------------
// Effects on ndsp 0-7 (Forsaken's pool); music on 23 (Forsaken's music channel)
// and 22 (the outgoing track of a crossfade), outside the pool.
const int32_t kMusicNdsp[VoiceBinder::kMusicChannels] = {23, 22};

int32_t ndspOf(int32_t c) {
    return c < VoiceBinder::kFirstMusicChannel ? c : kMusicNdsp[c - VoiceBinder::kFirstMusicChannel];
}

// ---- the sample arena (effects) --------------------------------------------
// 2 MiB of linear memory, reserved once. A level's effects archives
// (TRAVERSE.SNI + LEVELnS.SNI) are 0.83-0.98 MB on levels 3, 7 and 8, mostly
// 8-bit, which the arena holds as PCM8 at about the archive's own size; the
// first-fit arena with LRU eviction keeps what the level is playing when a level
// plays more than that. Against ~20.7 MB of linear heap free at the loop's start
// on the console (mem[loop] lin=20685696, logs/hw 2026-11-17).
constexpr uint32_t kSampleArenaBytes = 2u * 1024u * 1024u;
constexpr int32_t kMaxEntries = 192;
constexpr uint32_t kArenaAlign = 32u;  // a cache line: each entry flushes its own lines

struct Entry {
    bool used = false;
    std::string name;
    uint32_t offset = 0;
    uint32_t bytes = 0;
    int32_t frames = 0;
    int32_t channels = 0;
    bool pcm8 = false;
    uint32_t lastUse = 0;
    uint32_t releasedAt = 0;  // the frame a channel last let go of it
};

struct EffectChannel {
    ndspWaveBuf wave[2];   // [0] the sound (or a looping rebind's tail), [1] the loop
    int32_t entry = -1;    // the arena entry the channel plays or last played
};

// ---- the music streams ---------------------------------------------------------
// THE LEVEL'S MUSIC IS DSP-ADPCM FROM ITS OWN FILES (mdk_3ds/music/levelN/<name>.dsp,
// romfs:/music/levelN on the console; mdk_3ds/tools/music_dsp_convert.py makes
// them). The bank never reads or decodes a track (SoundBank::addStreamedMusic):
// the worker reads it, a wave buffer at a time, and the DSP decodes it. Forsaken's
// and T2K's shape (vault 10-projects/forsaken/findings/dsp-adpcm-music-and-sfx-audio.md):
// a queue of ADPCM wave buffers per channel, each handed the predictor history
// the previous one ended with (audio/dsp_adpcm.h).
//
// THE BUFFERING IS THE READ-AHEAD, AND IT IS GENEROUS (owner, 2026-11-25: "we
// shouldnt be so stingy with memory that we arent holding an adequate buffer for
// the music"). 24 buffers of 768 frames (10,752 samples, 0.336 s at 32 kHz, 6,144
// bytes) per channel: eight seconds queued in the DSP, so the worker can miss
// ~7.7 s of reads before a channel runs dry. Measured on the Old 3DS walks: the
// slowest single buffer read 148 ms, an unprimed start 105-145 ms, the HD set's
// toggle-on hold 12 s (the menu is up and the reader thread is busy, but the music
// worker reads its own files and kept up). 2 x 24 x 6,144 = 288 KiB of linear
// memory (the PCM16 queue it replaces was 128 KiB for 0.74 s). A start queues
// the first two seconds (kMusicPrimeBuffers: primed, or read at once); the worker
// tops the queue up kMusicRefillPerPass buffers a pass from there.
//
// NO READ AT A TRACK'S START. A new track starts from its beginning the frame
// LevelMusic asks for it, with no read on that frame: the arena that is loading
// (and the door bubble's nearest) has its track PRIMED by the worker ahead of
// time (audioDspPrimeMusic, from the frame's existing prefetch hints) -- the
// file opened and its first two seconds read into main memory -- and a start
// copies the prime into the channel's buffers. A start nobody primed reads on
// the worker (said: "music: ... not primed").
//
// WHO TOUCHES WHAT. The worker alone opens, reads and closes the files and
// fills the wave buffers, and it reads OUTSIDE g_lock, so a slow card never
// blocks the main thread's frame. The main thread posts requests under g_lock
// (start, stop, the mix) and signals the worker, which applies them at once.
constexpr int32_t kMusicBuffers = 24;
constexpr int32_t kMusicPrimeBuffers = 6;
constexpr int32_t kMusicRefillPerPass = 6;
constexpr int32_t kMusicBufFrames = 768;
constexpr int32_t kMusicBufSamples = kMusicBufFrames * audio::DspAdpcm::kFrameSamples;
constexpr uint32_t kMusicBufBytes = (uint32_t)kMusicBufFrames * audio::DspAdpcm::kFrameBytes;
constexpr uint32_t kMusicBytes =
    (uint32_t)VoiceBinder::kMusicChannels * kMusicBuffers * kMusicBufBytes;
constexpr int32_t kMusicPrimes = 2;
constexpr int32_t kMusicNameBytes = 16;
constexpr int32_t kMusicPathBytes = 64;
constexpr float kMusicCutoff = 6000.0f;          // AudioDevice.MusicCutoff

// An open track: the worker's alone.
struct MusicFile {
    FILE* file = nullptr;
    char path[kMusicPathBytes] = {0};
    audio::DspAdpcmHeader head;
    uint32_t next = 0;               // the next sample to queue
    audio::DspAdpcmContext history;  // the predictor history at `next`
    // The ADPCM bytes the file holds after its header. gc-dspadpcm-encode writes
    // the LAST frame only as far as its samples need (a 92,308-sample track ends
    // 4 bytes short of whole frames), so the end is read to the file's end and
    // the rest of that frame is zeros (silence the DSP never plays: nsamples
    // stops at the track's length).
    uint32_t dataBytes = 0;
    uint32_t at = 0;  // the data byte the file is at (`next`'s frame)
};

// A track's first two seconds read ahead (audioDspPrimeMusic).
struct MusicPrime {
    char name[kMusicNameBytes] = {0};  // guarded by g_lock: what is wanted (empty: nothing)
    // ---- the worker's ----
    char loaded[kMusicNameBytes] = {0};  // what `stream` and `data` hold
    // Where the primed bytes end: path, header, sample and history there. Its
    // file is CLOSED once the prime is read, so a prime is not used up by a start
    // (the start opens its own file at that place): a track the door bubble
    // hinted stays primed for every return through that door.
    MusicFile stream;
    uint8_t* data = nullptr;           // kMusicPrimeBuffers x kMusicBufBytes (main memory)
    uint32_t bufferSamples[kMusicPrimeBuffers] = {0};
    audio::DspAdpcmContext bufferHistory[kMusicPrimeBuffers];
    int32_t buffers = 0;
};

struct MusicSlot {
    ndspWaveBuf wave[kMusicBuffers];
    ndspAdpcmData history[kMusicBuffers];
    uint8_t* buf = nullptr;  // linear: kMusicBuffers x kMusicBufBytes
    // ---- guarded by g_lock: the request (main thread) ----
    uint32_t serial = 0;     // bumped by every start and stop
    bool want = false;       // a start (else a stop)
    bool cut = false;        // a stop that silences what is queued
    char name[kMusicNameBytes] = {0};
    bool looping = false;
    int32_t startFrame = 0;
    float rate = 0.0f;
    float left = 0.0f;
    float right = 0.0f;
    // ---- guarded by g_lock: the worker's state the main thread reads ----
    uint32_t applied = 0;    // the serial the worker last carried out
    bool active = false;     // the worker keeps this channel fed
    int32_t silentTicks = 0; // worker ticks in a row the channel was not playing
    // ---- the worker's ----
    MusicFile stream;
    uint32_t started = 0;      // the serial of the last start carried out
    uint32_t restarts = 0;     // this channel's drained restarts (underruns)
    bool restartAllBack = false;   // the last one: every buffer had come back
    uint32_t restartAt = 0;        // ... the track's next sample then
    uint32_t restartSamples = 0;   // ... and the track's length
    bool startPrimed = false;  // the last start came from a prime
    uint32_t startUs = 0;      // ... and took this long on the worker
    // ---- main thread only ----
    bool owned = false;      // a logical voice is (or was, until stopped) streaming here
    uint32_t said = 0;       // the serial whose start the log has said
    uint32_t restartsSaid = 0;
};

bool g_live = false;
VoiceBinder g_binder;
EffectChannel g_effects[VoiceBinder::kEffectChannels];
MusicSlot g_music[VoiceBinder::kMusicChannels];
uint8_t* g_arena = nullptr;
uint8_t* g_musicPool = nullptr;
MusicPrime g_primes[kMusicPrimes];
int32_t g_primeNext = 0;  // guarded by g_lock: the prime the next wish takes
char g_musicFolder[kMusicPathBytes] = {0};
Entry g_entries[kMaxEntries];
uint32_t g_useSerial = 0;
uint32_t g_frame = 0;
float g_lowPassE = 0.0f;  // exp(-2 pi fc / fs) at the DSP's rate

LightLock g_lock;
bool g_attached = false;  // main thread: the streaming worker serves the music
bool g_kick = false;      // guarded by g_lock: a request the worker should carry out now
bool g_suspended = false; // guarded by g_lock: the system is suspended / asleep
int32_t g_resumeGrace = 0;  // guarded by g_lock: ticks after a resume with no restart
aptHookCookie g_aptCookie;

uint32_t g_cacheFills = 0;
uint32_t g_cacheEvictions = 0;
uint32_t g_cacheMisses = 0;
uint64_t g_fillTicks = 0;     // main thread: the arena copies' time (arenaGet), all of it
uint64_t g_fillTicksMax = 0;  // ... and the slowest one
uint32_t g_musicQueued = 0;   // guarded by g_lock
uint32_t g_musicRestarts = 0; // guarded by g_lock
uint32_t g_musicUnprimed = 0; // guarded by g_lock: starts that had to read first
uint32_t g_musicMissing = 0;  // guarded by g_lock: tracks whose file did not open
uint32_t g_musicReadMax = 0;  // guarded by g_lock: the slowest buffer read, microseconds

// ---- the arena --------------------------------------------------------------

uint32_t arenaUsed() {
    uint32_t used = 0;
    for (int32_t i = 0; i < kMaxEntries; i++) {
        used += g_entries[i].used ? g_entries[i].bytes : 0u;
    }
    return used;
}

bool entryInUse(int32_t e) {
    for (int32_t c = 0; c < VoiceBinder::kEffectChannels; c++) {
        if (g_effects[c].entry == e) {
            return true;
        }
    }
    // A channel let go of it within the last two frames: ndspChnWaveBufClear
    // unlinks the buffer, but the DSP may still be reading the frame it began.
    return g_frame - g_entries[e].releasedAt < 2u;
}

// The lowest offset where `bytes` fit between the used entries, or -1. Cold (a
// sound's first play): a quadratic scan over at most kMaxEntries.
int64_t findGap(uint32_t bytes) {
    for (int32_t cand = -1; cand < kMaxEntries; cand++) {
        uint32_t start = 0;
        if (cand >= 0) {
            if (!g_entries[cand].used) {
                continue;
            }
            start = g_entries[cand].offset + g_entries[cand].bytes;
        }
        if (start + bytes > kSampleArenaBytes) {
            continue;
        }
        bool clear = true;
        for (int32_t i = 0; i < kMaxEntries && clear; i++) {
            const Entry& e = g_entries[i];
            clear = !e.used || e.offset >= start + bytes || e.offset + e.bytes <= start;
        }
        if (clear) {
            return (int64_t)start;
        }
    }
    return -1;
}

// Evict the least recently played entry nothing is playing. False: none.
bool evictOne() {
    int32_t victim = -1;
    for (int32_t i = 0; i < kMaxEntries; i++) {
        if (g_entries[i].used && !entryInUse(i) &&
            (victim < 0 || g_entries[i].lastUse < g_entries[victim].lastUse)) {
            victim = i;
        }
    }
    if (victim < 0) {
        return false;
    }
    g_entries[victim].used = false;
    g_entries[victim].name.clear();
    g_cacheEvictions++;
    return true;
}

// The arena entry holding `sound` (named `name`), copying it in on first use.
// -1 when it cannot be placed: the voice then plays silent, it does not end.
int32_t arenaGet(const std::string& name, const audio::Sound& sound) {
    const int32_t channels = sound.channels > 0 ? sound.channels : 1;
    const int32_t frames = sound.frames();
    for (int32_t i = 0; i < kMaxEntries; i++) {
        const Entry& e = g_entries[i];
        if (e.used && e.frames == frames && e.channels == channels && e.name == name) {
            return i;
        }
    }
    const size_t count = (size_t)frames * (size_t)channels;
    if (count == 0) {
        return -1;
    }
    // 8-bit sounds are stored as (byte-128)*256 (sound.h): a zero low byte on
    // every sample means the PCM8 copy is the same values, in half the bytes.
    const int16_t* src = sound.samples.data();
    bool pcm8 = true;
    for (size_t k = 0; k < count && pcm8; k++) {
        pcm8 = (src[k] & 0xFF) == 0;
    }
    const uint32_t bytes = (uint32_t)(pcm8 ? count : count * 2u);
    const uint32_t span = (bytes + kArenaAlign - 1u) & ~(kArenaAlign - 1u);
    if (span > kSampleArenaBytes) {
        g_cacheMisses++;
        return -1;
    }
    int32_t slot = -1;
    for (int32_t i = 0; i < kMaxEntries && slot < 0; i++) {
        slot = g_entries[i].used ? -1 : i;
    }
    int64_t at = findGap(span);
    // Each eviction frees an entry, so this ends after at most kMaxEntries.
    for (int32_t n = 0; n < kMaxEntries && (at < 0 || slot < 0); n++) {
        if (!evictOne()) {
            break;
        }
        for (int32_t i = 0; i < kMaxEntries && slot < 0; i++) {
            slot = g_entries[i].used ? -1 : i;
        }
        at = findGap(span);
    }
    if (at < 0 || slot < 0) {
        g_cacheMisses++;
        return -1;
    }
    uint8_t* dst = g_arena + (uint32_t)at;
    const u64 t0 = svcGetSystemTick();
    if (pcm8) {
        int8_t* out = (int8_t*)dst;
        for (size_t k = 0; k < count; k++) {
            out[k] = (int8_t)(src[k] / 256);  // exact: the value is a multiple of 256
        }
    } else {
        memcpy(dst, src, count * sizeof(int16_t));
    }
    DSP_FlushDataCache(dst, bytes);
    const u64 ticks = svcGetSystemTick() - t0;
    g_fillTicks += ticks;
    g_fillTicksMax = ticks > g_fillTicksMax ? ticks : g_fillTicksMax;
    Entry& e = g_entries[slot];
    e.used = true;
    e.name = name;
    e.offset = (uint32_t)at;
    e.bytes = span;
    e.frames = frames;
    e.channels = channels;
    e.pcm8 = pcm8;
    e.lastUse = 0;
    e.releasedAt = g_frame - 2u;
    g_cacheFills++;
    return slot;
}

// ---- the effect channels ------------------------------------------------------

void setMix(int32_t ndsp, float left, float right) {
    float mix[12] = {};
    mix[0] = left;
    mix[1] = right;
    ndspChnSetMix(ndsp, mix);
}

void effectRelease(int32_t c) {
    EffectChannel& hw = g_effects[c];
    if (hw.entry >= 0) {
        g_entries[hw.entry].releasedAt = g_frame;
        hw.entry = -1;
    }
}

void effectCut(int32_t c) {
    ndspChnWaveBufClear(c);
    effectRelease(c);
}

void effectStart(int32_t c, const VoiceBinder::Channel& order, const std::string& name) {
    EffectChannel& hw = g_effects[c];
    ndspChnWaveBufClear(c);
    effectRelease(c);
    const int32_t e = arenaGet(name, *order.sound);
    if (e < 0) {
        return;  // silent: the logical voice plays on, unheard
    }
    Entry& entry = g_entries[e];
    entry.lastUse = ++g_useSerial;
    hw.entry = e;

    ndspChnReset(c);
    ndspChnSetInterp(c, NDSP_INTERP_LINEAR);
    ndspChnSetFormat(c, entry.channels == 2
                            ? (entry.pcm8 ? NDSP_FORMAT_STEREO_PCM8 : NDSP_FORMAT_STEREO_PCM16)
                            : (entry.pcm8 ? NDSP_FORMAT_MONO_PCM8 : NDSP_FORMAT_MONO_PCM16));
    ndspChnSetRate(c, order.rate);
    setMix(c, order.left, order.right);

    uint8_t* base = g_arena + entry.offset;
    const uint32_t frameBytes = (uint32_t)entry.channels * (entry.pcm8 ? 1u : 2u);
    int32_t start = order.startFrame;
    if (start < 0 || start >= entry.frames) {
        start = 0;
    }
    memset(hw.wave, 0, sizeof(hw.wave));
    // nsamples is in FRAMES (audio_3ds.cpp measured it for a stereo format);
    // status is FREE at the call, or ndspChnWaveBufAdd links nothing (ibid.).
    hw.wave[0].data_vaddr = base + (uint32_t)start * frameBytes;
    hw.wave[0].nsamples = (u32)(entry.frames - start);
    hw.wave[0].looping = order.looping && start == 0;
    hw.wave[0].status = NDSP_WBUF_FREE;
    ndspChnWaveBufAdd(c, &hw.wave[0]);
    if (order.looping && start > 0) {
        // A loop resumed mid-sound: the tail once, then the whole sound looping
        // (the PD port's pre-loop + loop-region pair).
        hw.wave[1].data_vaddr = base;
        hw.wave[1].nsamples = (u32)entry.frames;
        hw.wave[1].looping = true;
        hw.wave[1].status = NDSP_WBUF_FREE;
        ndspChnWaveBufAdd(c, &hw.wave[1]);
    }
}

// ---- the music channels ---------------------------------------------------------

// Reset the channel and configure it for the slot's track. Under g_lock.
void musicConfigure(int32_t ndsp, const MusicSlot& slot) {
    ndspChnReset(ndsp);
    ndspChnSetInterp(ndsp, NDSP_INTERP_LINEAR);
    ndspChnSetFormat(ndsp, NDSP_FORMAT_MONO_ADPCM);
    ndspChnSetAdpcmCoefs(ndsp, (u16*)slot.stream.head.coefs);
    ndspChnSetRate(ndsp, slot.rate);
    setMix(ndsp, slot.left, slot.right);
    // The music bus's one-pole low-pass (AudioDevice.AddMusic: y += (x - y) * a,
    // a = 1 - e), as a biquad in RBJ form with b1 = b2 = a2 = 0 (header).
    ndspChnIirBiquadSetParamsCustomFilter(ndsp, 1.0f, -g_lowPassE, 0.0f, 1.0f - g_lowPassE, 0.0f,
                                          0.0f);
    ndspChnIirBiquadSetEnable(ndsp, true);
}

// ---- the worker's file side (no lock held) ---------------------------------------

void musicClose(MusicFile& f) {
    if (f.file != nullptr) {
        fclose(f.file);
        f.file = nullptr;
    }
}

// Open `name`'s track (g_musicFolder/<name in lower case>.dsp) at its start.
bool musicOpen(MusicFile& f, const char* name) {
    musicClose(f);
    int32_t n = snprintf(f.path, sizeof(f.path), "%s/", g_musicFolder);
    for (int32_t i = 0; name[i] != '\0' && n < kMusicPathBytes - 5; i++, n++) {
        const char c = name[i];
        f.path[n] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    snprintf(f.path + n, sizeof(f.path) - (size_t)n, ".dsp");
    f.file = fopen(f.path, "rb");
    if (f.file == nullptr) {
        return false;
    }
    uint8_t header[audio::DspAdpcm::kHeaderBytes];
    if (fread(header, 1, sizeof(header), f.file) != sizeof(header) ||
        !audio::DspAdpcm::parseHeader(header, (int32_t)sizeof(header), &f.head)) {
        musicClose(f);
        return false;
    }
    // The data's size from the file, never from the header (dsp_adpcm.h).
    if (fseek(f.file, 0, SEEK_END) != 0) {
        musicClose(f);
        return false;
    }
    const long end = ftell(f.file);
    if (end <= audio::DspAdpcm::kHeaderBytes ||
        fseek(f.file, audio::DspAdpcm::kHeaderBytes, SEEK_SET) != 0) {
        musicClose(f);
        return false;
    }
    f.dataBytes = (uint32_t)(end - audio::DspAdpcm::kHeaderBytes);
    f.at = 0;
    f.next = 0;
    f.history.index = f.head.predScale;
    f.history.history0 = f.head.history0;
    f.history.history1 = f.head.history1;
    return true;
}

// Move to the frame holding sample `at` (a voice rebound mid-track; the history
// there is not known, so it restarts from silence: a click, never a crash).
void musicSeek(MusicFile& f, int32_t at) {
    if (f.file == nullptr || at <= 0 || (uint32_t)at >= f.head.samples) {
        return;
    }
    const uint32_t frame = (uint32_t)at / (uint32_t)audio::DspAdpcm::kFrameSamples;
    if (fseek(f.file, (long)(audio::DspAdpcm::kHeaderBytes + frame * audio::DspAdpcm::kFrameBytes),
              SEEK_SET) == 0) {
        f.next = frame * (uint32_t)audio::DspAdpcm::kFrameSamples;
        f.at = frame * (uint32_t)audio::DspAdpcm::kFrameBytes;
        f.history.history0 = 0;
        f.history.history1 = 0;
    }
}

// Read the track's next buffer into `dst`: its sample count (0: the track has
// ended and does not loop, or the read failed) and the history it starts from.
// A looping track that has ended starts again from its beginning (reopened, as
// Forsaken does: libctru's romfs FILE* can latch EOF across a seek).
uint32_t musicRead(MusicFile& f, bool looping, uint8_t* dst, audio::DspAdpcmContext* start) {
    if (f.file == nullptr) {
        return 0;
    }
    if (f.next >= f.head.samples) {
        if (!looping) {
            return 0;
        }
        char path[kMusicPathBytes];
        memcpy(path, f.path, sizeof(path));
        fclose(f.file);
        f.file = fopen(path, "rb");
        if (f.file == nullptr || fseek(f.file, audio::DspAdpcm::kHeaderBytes, SEEK_SET) != 0) {
            musicClose(f);
            return 0;
        }
        f.next = 0;
        f.at = 0;
        f.history.history0 = f.head.history0;
        f.history.history1 = f.head.history1;
    }
    uint32_t samples = f.head.samples - f.next;
    if (samples > (uint32_t)kMusicBufSamples) {
        samples = (uint32_t)kMusicBufSamples;
    }
    const uint32_t frames = audio::DspAdpcm::framesFor(samples);
    const uint32_t bytes = frames * audio::DspAdpcm::kFrameBytes;
    const u64 t0 = svcGetSystemTick();
    // What the file holds from here (the last frame may be short: dataBytes).
    const uint32_t held = f.at < f.dataBytes ? f.dataBytes - f.at : 0u;
    const uint32_t want = bytes < held ? bytes : held;
    const size_t got = fread(dst, 1, want, f.file);
    const uint32_t us = (uint32_t)((svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 1000000ull));
    if (got != want || want == 0) {
        return 0;
    }
    f.at += want;
    if (want < bytes) {
        memset(dst + want, 0, bytes - want);
    }
    DSP_FlushDataCache(dst, bytes);
    *start = f.history;
    start->index = dst[0];
    audio::DspAdpcm::advance(dst, (int32_t)frames, f.head.coefs, &f.history);
    f.next += samples;
    LightLock_Lock(&g_lock);
    if (us > g_musicReadMax) {
        g_musicReadMax = us;
    }
    LightLock_Unlock(&g_lock);
    return samples;
}

// Hand buffer i to the DSP. Under g_lock.
void musicQueue(int32_t ndsp, MusicSlot& slot, int32_t i, uint32_t samples,
                const audio::DspAdpcmContext& start) {
    slot.history[i].index = start.index;
    slot.history[i].history0 = start.history0;
    slot.history[i].history1 = start.history1;
    ndspWaveBuf& wb = slot.wave[i];
    memset(&wb, 0, sizeof(wb));
    wb.data_adpcm = slot.buf + (size_t)i * kMusicBufBytes;
    wb.nsamples = samples;
    wb.adpcm_data = &slot.history[i];
    wb.looping = false;
    wb.status = NDSP_WBUF_FREE;
    ndspChnWaveBufAdd(ndsp, &wb);
    g_musicQueued++;
}

// A request the worker carries out (a copy taken under g_lock).
struct MusicRequest {
    uint32_t serial = 0;
    bool want = false;
    bool cut = false;
    char name[kMusicNameBytes] = {0};
    bool looping = false;
    int32_t startFrame = 0;
};

// The prime holding `name`'s start, or -1. The worker's.
int32_t musicPrimeOf(const char* name) {
    for (int32_t p = 0; p < kMusicPrimes; p++) {
        if (g_primes[p].buffers > 0 && strcmp(g_primes[p].loaded, name) == 0) {
            return p;
        }
    }
    return -1;
}

// A start: the track opened (from its prime, or read now), the buffers filled
// and queued. The worker's; takes g_lock only to hand the result over.
void musicApplyStart(int32_t m, const MusicRequest& req) {
    MusicSlot& slot = g_music[m];
    const int32_t ndsp = kMusicNdsp[m];
    uint32_t samples[kMusicBuffers] = {0};
    audio::DspAdpcmContext starts[kMusicBuffers];
    int32_t filled = 0;
    const int32_t p = req.startFrame == 0 ? musicPrimeOf(req.name) : -1;
    const u64 t0 = svcGetSystemTick();
    if (p >= 0) {
        // The prime becomes the stream: its file (positioned after the primed
        // bytes) and its buffers, copied into the channel's.
        MusicPrime& prime = g_primes[p];
        musicClose(slot.stream);
        slot.stream = prime.stream;
        slot.stream.file = fopen(slot.stream.path, "rb");
        if (slot.stream.file != nullptr &&
            fseek(slot.stream.file,
                  (long)(audio::DspAdpcm::kHeaderBytes + slot.stream.at), SEEK_SET) != 0) {
            musicClose(slot.stream);
        }
        for (int32_t i = 0; i < prime.buffers; i++) {
            memcpy(slot.buf + (size_t)i * kMusicBufBytes, prime.data + (size_t)i * kMusicBufBytes,
                   kMusicBufBytes);
            samples[i] = prime.bufferSamples[i];
            starts[i] = prime.bufferHistory[i];
        }
        DSP_FlushDataCache(slot.buf, (u32)prime.buffers * kMusicBufBytes);
        filled = prime.buffers;
    } else if (musicOpen(slot.stream, req.name)) {
        musicSeek(slot.stream, req.startFrame);
        for (int32_t i = 0; i < kMusicPrimeBuffers; i++) {
            samples[i] = musicRead(slot.stream, req.looping, slot.buf + (size_t)i * kMusicBufBytes,
                                   &starts[i]);
            if (samples[i] == 0) {
                break;
            }
            filled++;
        }
    }
    const uint32_t us = (uint32_t)((svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 1000000ull));
    LightLock_Lock(&g_lock);
    if (p < 0) {
        g_musicUnprimed++;
    }
    if (filled == 0) {
        g_musicMissing += slot.stream.file == nullptr ? 1u : 0u;
    }
    // A newer request, or the system suspended meanwhile: this start is not
    // handed over (the next pass carries out the newer request, or this one again).
    if (slot.serial == req.serial && !g_suspended) {
        slot.applied = req.serial;
        slot.started = req.serial;
        slot.startPrimed = p >= 0;
        slot.startUs = us;
        slot.active = filled > 0;
        slot.silentTicks = 0;
        if (slot.active) {
            musicConfigure(ndsp, slot);
            for (int32_t i = 0; i < kMusicBuffers; i++) {
                slot.wave[i].status = NDSP_WBUF_FREE;
            }
            for (int32_t i = 0; i < filled; i++) {
                musicQueue(ndsp, slot, i, samples[i], starts[i]);
            }
        }
    }
    LightLock_Unlock(&g_lock);
}

// One worker pass over one music channel.
void musicWork(int32_t m) {
    MusicSlot& slot = g_music[m];
    const int32_t ndsp = kMusicNdsp[m];
    MusicRequest req;
    bool fresh = false;
    bool refill[kMusicBuffers] = {false};
    bool restart = false;
    bool restartAllBack = false;
    bool looping = false;
    uint32_t serial = 0;
    LightLock_Lock(&g_lock);
    if (g_suspended) {
        LightLock_Unlock(&g_lock);
        return;
    }
    if (slot.serial != slot.applied) {
        fresh = true;
        req.serial = slot.serial;
        req.want = slot.want;
        req.cut = slot.cut;
        memcpy(req.name, slot.name, sizeof(req.name));
        req.looping = slot.looping;
        req.startFrame = slot.startFrame;
        if (req.cut) {
            ndspChnWaveBufClear(ndsp);
        }
        slot.active = false;
        if (!req.want) {
            slot.applied = req.serial;
        }
    } else if (slot.active) {
        bool drained = true;
        for (int32_t i = 0; i < kMusicBuffers; i++) {
            const u8 st = slot.wave[i].status;
            drained = drained && (st == NDSP_WBUF_DONE || st == NDSP_WBUF_FREE);
            refill[i] = st == NDSP_WBUF_DONE || st == NDSP_WBUF_FREE;
        }
        slot.silentTicks = ndspChnIsPlaying(ndsp) ? 0 : slot.silentTicks + 1;
        // THE DRAINED-CHANNEL TRAP (Forsaken music_3ds.cpp, vault finding A): ndsp
        // does not resume a channel that ran dry when buffers are added to it, so it
        // is cleared, re-configured and primed again from where the stream is. Two
        // ways to be dry: every buffer is back, or the channel has not been playing
        // for two ticks in a row (the last buffer finished between this tick's
        // status read and its refills). Not on one silent tick (a freshly queued
        // channel is not yet marked playing) and not in a resume's grace ticks.
        // Every restart is an UNDERRUN: the reads fell more than the queue behind.
        restart = (drained || slot.silentTicks >= 2) && g_resumeGrace == 0;
        restartAllBack = drained;
        if (restart) {
            slot.silentTicks = 0;
            ndspChnWaveBufClear(ndsp);
            for (int32_t i = 0; i < kMusicBuffers; i++) {
                slot.wave[i].status = NDSP_WBUF_FREE;
                refill[i] = true;
            }
        }
        // A pass tops up at most kMusicRefillPerPass buffers (two seconds), so a
        // pass never holds the worker for a long run of reads.
        int32_t allowed = kMusicRefillPerPass;
        for (int32_t i = 0; i < kMusicBuffers; i++) {
            if (refill[i] && allowed > 0) {
                allowed--;
            } else {
                refill[i] = false;
            }
        }
        looping = slot.looping;
        serial = slot.serial;
    }
    LightLock_Unlock(&g_lock);

    if (fresh) {
        musicClose(slot.stream);
        if (req.want) {
            musicApplyStart(m, req);
        }
        return;
    }
    uint32_t samples[kMusicBuffers] = {0};
    audio::DspAdpcmContext starts[kMusicBuffers];
    bool any = false;
    for (int32_t i = 0; i < kMusicBuffers; i++) {
        if (refill[i]) {
            samples[i] = musicRead(slot.stream, looping, slot.buf + (size_t)i * kMusicBufBytes,
                                   &starts[i]);
            any = any || samples[i] > 0;
        }
    }
    if (!restart && !any) {
        return;
    }
    LightLock_Lock(&g_lock);
    if (slot.serial == serial && slot.active && !g_suspended) {
        if (restart) {
            g_musicRestarts++;
            slot.restarts++;
            slot.restartAllBack = restartAllBack;
            slot.restartAt = slot.stream.next;
            slot.restartSamples = slot.stream.head.samples;
            if (any) {
                musicConfigure(ndsp, slot);
            } else {
                slot.active = false;  // a one-shot track played out
            }
        }
        for (int32_t i = 0; i < kMusicBuffers; i++) {
            if (samples[i] > 0) {
                musicQueue(ndsp, slot, i, samples[i], starts[i]);
            }
        }
    }
    LightLock_Unlock(&g_lock);
}

// The primes: each one's wished-for track opened and its first buffers read.
void musicPrimeWork() {
    for (int32_t p = 0; p < kMusicPrimes; p++) {
        MusicPrime& prime = g_primes[p];
        char want[kMusicNameBytes];
        LightLock_Lock(&g_lock);
        memcpy(want, prime.name, sizeof(want));
        LightLock_Unlock(&g_lock);
        if (strcmp(want, prime.loaded) == 0) {
            continue;
        }
        musicClose(prime.stream);
        prime.buffers = 0;
        memcpy(prime.loaded, want, sizeof(prime.loaded));
        if (want[0] == '\0' || prime.data == nullptr) {
            continue;
        }
        if (!musicOpen(prime.stream, want)) {
            LightLock_Lock(&g_lock);
            g_musicMissing++;
            LightLock_Unlock(&g_lock);
            continue;
        }
        for (int32_t i = 0; i < kMusicPrimeBuffers; i++) {
            prime.bufferSamples[i] = musicRead(prime.stream, true,
                                               prime.data + (size_t)i * kMusicBufBytes,
                                               &prime.bufferHistory[i]);
            if (prime.bufferSamples[i] == 0) {
                break;
            }
            prime.buffers++;
        }
        musicClose(prime.stream);
    }
}

} // namespace

// THE STREAMING WORKER'S PASS (stream_worker_3ds.h): on its 25 ms tick, or
// sooner when a start, stop or prime asked for it (g_kick).
void audioDspWorkerPass(bool tickDue) {
    LightLock_Lock(&g_lock);
    const bool kicked = g_kick;
    g_kick = false;
    LightLock_Unlock(&g_lock);
    if (!tickDue && !kicked) {
        return;
    }
    // Suspended (HOME, sleep): each pass checks under g_lock and leaves the
    // channels alone while ndsp saves and restores them (musicWork).
    for (int32_t m = 0; m < VoiceBinder::kMusicChannels; m++) {
        musicWork(m);
    }
    musicPrimeWork();
    if (tickDue) {
        LightLock_Lock(&g_lock);
        if (!g_suspended && g_resumeGrace > 0) {
            g_resumeGrace--;
        }
        LightLock_Unlock(&g_lock);
    }
}

namespace {

// libctru runs the hooks newest first, so this one (registered after ndspInit)
// runs BEFORE ndsp's own: on suspend the worker is parked before ndsp saves its
// channels, and on resume it comes back before ndsp has restored them -- which is
// why a resume opens a grace of four ticks (100 ms) in which the worker refills
// but never restarts a channel that only looks dry.
void onAptHook(APT_HookType hook, void*) {
    if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP) {
        LightLock_Lock(&g_lock);
        g_suspended = true;
        LightLock_Unlock(&g_lock);
    } else if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP) {
        LightLock_Lock(&g_lock);
        g_suspended = false;
        g_resumeGrace = 4;
        for (int32_t m = 0; m < VoiceBinder::kMusicChannels; m++) {
            g_music[m].silentTicks = 0;
        }
        LightLock_Unlock(&g_lock);
    }
}

// ---- the main thread's side: requests ------------------------------------------

// A request posted: the streaming worker's next pass carries it out.
void kickWorker() {
    LightLock_Lock(&g_lock);
    g_kick = true;
    LightLock_Unlock(&g_lock);
    streamWorkerWake();
}

void musicStop(int32_t m, bool cut) {
    MusicSlot& slot = g_music[m];
    LightLock_Lock(&g_lock);
    slot.serial++;
    slot.want = false;
    slot.cut = cut;
    LightLock_Unlock(&g_lock);
    slot.owned = false;
    kickWorker();
}

void musicStart(int32_t m, const VoiceBinder::Channel& order, const std::string& name) {
    MusicSlot& slot = g_music[m];
    LightLock_Lock(&g_lock);
    slot.serial++;
    slot.want = true;
    slot.cut = true;
    snprintf(slot.name, sizeof(slot.name), "%s", name.c_str());
    slot.looping = order.looping;
    slot.startFrame = order.startFrame;
    slot.rate = order.rate;
    slot.left = order.left;
    slot.right = order.right;
    LightLock_Unlock(&g_lock);
    slot.owned = true;
    // The streaming worker carries it out at once.
    kickWorker();
}

void musicMix(int32_t m, const VoiceBinder::Channel& order) {
    MusicSlot& slot = g_music[m];
    LightLock_Lock(&g_lock);
    slot.rate = order.rate;
    slot.left = order.left;
    slot.right = order.right;
    if (slot.active && !g_suspended) {
        setMix(kMusicNdsp[m], order.left, order.right);
        ndspChnSetRate(kMusicNdsp[m], order.rate);
    }
    LightLock_Unlock(&g_lock);
}

// The music lines the frame says (main thread): each start the worker carried
// out, primed or read, and every underrun.
void musicSay() {
    for (int32_t m = 0; m < VoiceBinder::kMusicChannels; m++) {
        MusicSlot& slot = g_music[m];
        LightLock_Lock(&g_lock);
        const bool fresh = slot.started != slot.said;
        const bool active = slot.active;
        const bool primed = slot.startPrimed;
        const uint32_t us = slot.startUs;
        const uint32_t restarts = slot.restarts;
        const bool allBack = slot.restartAllBack;
        const uint32_t at = slot.restartAt;
        const uint32_t length = slot.restartSamples;
        char name[kMusicNameBytes];
        memcpy(name, slot.name, sizeof(name));
        slot.said = slot.started;
        LightLock_Unlock(&g_lock);
        if (fresh) {
            log3dsPrintf("music: %s on ch%d: %s (%.1f ms on the worker)%s", name,
                         (int)kMusicNdsp[m], primed ? "primed" : "NOT primed, read at the start",
                         (double)us / 1000.0, active ? "" : " -- NO FILE, silent");
        }
        if (restarts != slot.restartsSaid) {
            log3dsPrintf("music: UNDERRUN on ch%d (%s): %u drained restart(s) on this channel; "
                         "the last: %s, at sample %u of %u",
                         (int)kMusicNdsp[m], name, (unsigned)restarts,
                         allBack ? "every buffer back" : "the channel silent two ticks",
                         (unsigned)at, (unsigned)length);
            slot.restartsSaid = restarts;
        }
    }
}

// The mixer's name for a voice (the arena and the pins key on it), or a stand-in
// built from the samples' address when the mixer does not know the voice.
std::string nameOf(const audio::SoundMixer& mixer, int32_t id, const audio::Sound* sound) {
    const std::vector<audio::SoundMixer::Voice>& voices = mixer.voices();
    for (size_t i = 0; i < voices.size(); i++) {
        if (voices[i].id == id) {
            return voices[i].name;
        }
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "#%p", (const void*)sound);
    return std::string(buf);
}

void cutAll() {
    for (int32_t c = 0; c < VoiceBinder::kEffectChannels; c++) {
        effectCut(c);
    }
    for (int32_t m = 0; m < VoiceBinder::kMusicChannels; m++) {
        musicStop(m, true);
    }
}

} // namespace

bool audioDspOpen() {
    // R1 on the main thread, which runs the binder and the channel calls: set at
    // the top of main already, so this is the idempotent read-back audioOpen()
    // also logs.
    const bool fzdn = vfpThreadFzDn();
    log3dsPrintf("audio: FZ/DN %d (fpscr=%08X)", fzdn ? 1 : 0, (unsigned)vfpReadFpscr());
    if (g_live) {
        return true;
    }
    if (access("sdmc:/3ds/dspfirm.cdc", F_OK) != 0) {
        log3dsPrintf("audio: sdmc:/3ds/dspfirm.cdc is missing — running muted");
        return false;
    }
    const Result rc = ndspInit();
    if (rc != 0) {
        log3dsPrintf("audio: ndspInit failed %08X — running muted", (unsigned)rc);
        return false;
    }
    ndspSetMasterVol(1.0f);
    g_arena = (uint8_t*)linearAlloc(kSampleArenaBytes);
    g_musicPool = (uint8_t*)linearAlloc(kMusicBytes);
    // The primes' buffers: main memory (the worker copies them into the linear
    // wave buffers at a start).
    for (int32_t p = 0; p < kMusicPrimes; p++) {
        g_primes[p] = MusicPrime();
        g_primes[p].data = (uint8_t*)malloc((size_t)kMusicPrimeBuffers * kMusicBufBytes);
    }
    if (g_arena == nullptr || g_musicPool == nullptr) {
        log3dsPrintf("audio: linearAlloc(%u + %u) failed — running muted",
                     (unsigned)kSampleArenaBytes, (unsigned)kMusicBytes);
        if (g_arena != nullptr) {
            linearFree(g_arena);
            g_arena = nullptr;
        }
        if (g_musicPool != nullptr) {
            linearFree(g_musicPool);
            g_musicPool = nullptr;
        }
        for (int32_t p = 0; p < kMusicPrimes; p++) {
            free(g_primes[p].data);
            g_primes[p].data = nullptr;
        }
        ndspExit();
        return false;
    }
    /* The music low-pass's pole at the DSP's own rate: one expf at open. */
    g_lowPassE = expf(-2.0f * 3.14159265f * kMusicCutoff / (float)NDSP_SAMPLE_RATE);
    for (int32_t i = 0; i < kMaxEntries; i++) {
        g_entries[i] = Entry();
    }
    for (int32_t c = 0; c < VoiceBinder::kEffectChannels; c++) {
        memset(g_effects[c].wave, 0, sizeof(g_effects[c].wave));
        g_effects[c].entry = -1;
        ndspChnReset(c);
    }
    for (int32_t m = 0; m < VoiceBinder::kMusicChannels; m++) {
        g_music[m] = MusicSlot();
        memset(g_music[m].wave, 0, sizeof(g_music[m].wave));
        g_music[m].buf = g_musicPool + (size_t)m * kMusicBuffers * kMusicBufBytes;
        ndspChnReset(kMusicNdsp[m]);
    }
    g_binder.reset();
    g_frame = 0;

    LightLock_Init(&g_lock);
    g_kick = false;
    g_suspended = false;
    g_resumeGrace = 0;
    aptHook(&g_aptCookie, onAptHook, nullptr);
    // THE MUSIC IS THE STREAMING WORKER'S (stream_worker_3ds.h), on core 1:
    // measured 2026-11-25 on the Old 3DS appliance, on the main thread's core a
    // track's start and a prime's reads ran inside the door-crossing frame.
    // Without the worker the music does not stream (said below).
    g_attached = streamWorkerRunning();
    streamWorkerAttachAudio(g_attached);
    g_live = true;
    log3dsPrintf("audio: dsp voices=%d music=ch%d+ch%d (crossfade) effects=ndsp 0-%d; arena %u B "
                 "linear (samples, PCM8 for 8-bit), music DSP-ADPCM from %s, %d x %d x %d "
                 "ADPCM frames = %u B linear, %d primes; "
                 "music %s; music low-pass biquad %.0f Hz "
                 "(e=%.6f at %.1f Hz); no limiter; linear free after %u B. The `audio: frame=` "
                 "sink ledger counts the software sink and reads 0 in this mode: read `audio: dsp`",
                 (int)VoiceBinder::kEffectChannels, (int)kMusicNdsp[0], (int)kMusicNdsp[1],
                 (int)VoiceBinder::kEffectChannels - 1, (unsigned)kSampleArenaBytes,
                 g_musicFolder[0] ? g_musicFolder : "(no folder)",
                 (int)VoiceBinder::kMusicChannels, (int)kMusicBuffers, (int)kMusicBufFrames,
                 (unsigned)kMusicBytes, (int)kMusicPrimes,
                 g_attached ? "streamed by the streaming worker, 25 ms tick"
                            : "NOT streamed (no streaming worker): silent",
                 (double)kMusicCutoff,
                 (double)g_lowPassE, (double)NDSP_SAMPLE_RATE, (unsigned)linearSpaceFree());
    return true;
}

void audioDspClose() {
    if (!g_live) {
        return;
    }
    aptUnhook(&g_aptCookie);
    // The session's summary, read off the counters before they go: the music's
    // underruns and slowest read, the DSP's dropped frames, and what the effect
    // copies cost the main thread.
    {
        const AudioDspStats st = audioDspStats();
        const double msPerTick = 1.0 / (double)(SYSCLOCK_ARM11 / 1000ull);
        log3dsPrintf("audio: dsp close: music %u buffers queued, %u underrun restart(s), %u unprimed "
                     "start(s), slowest buffer read %.1f ms; dsp frames %u dropped %u; effect "
                     "copies %u (%u evicted, %u missed) %.1f ms on main, slowest %.2f ms",
                     (unsigned)st.musicQueued, (unsigned)st.musicRestarts,
                     (unsigned)st.musicUnprimed, (double)st.musicReadMaxUs / 1000.0,
                     (unsigned)st.dspFrames, (unsigned)st.dspDropped, (unsigned)g_cacheFills,
                     (unsigned)g_cacheEvictions, (unsigned)g_cacheMisses,
                     (double)g_fillTicks * msPerTick, (double)g_fillTicksMax * msPerTick);
        g_fillTicks = 0;
        g_fillTicksMax = 0;
    }
    // P4: detached, the worker is out of the music's pass for good -- then
    // nothing it reads may go.
    if (g_attached) {
        streamWorkerAttachAudio(false);
        g_attached = false;
    }
    // The worker has let go: the channels are silenced and the files closed here.
    for (int32_t c = 0; c < VoiceBinder::kEffectChannels; c++) {
        effectCut(c);
    }
    for (int32_t m = 0; m < VoiceBinder::kMusicChannels; m++) {
        ndspChnWaveBufClear(kMusicNdsp[m]);
        musicClose(g_music[m].stream);
        g_music[m].active = false;
        g_music[m].owned = false;
    }
    for (int32_t p = 0; p < kMusicPrimes; p++) {
        musicClose(g_primes[p].stream);
        free(g_primes[p].data);
        g_primes[p] = MusicPrime();
    }
    g_binder.reset();
    // The folder was this opening's (a level's, the menu's): the next names its own.
    g_musicFolder[0] = '\0';
    ndspExit();
    // The DSP has stopped: its memory can go.
    linearFree(g_arena);
    g_arena = nullptr;
    linearFree(g_musicPool);
    g_musicPool = nullptr;
    for (int32_t m = 0; m < VoiceBinder::kMusicChannels; m++) {
        g_music[m].buf = nullptr;
    }
    for (int32_t i = 0; i < kMaxEntries; i++) {
        g_entries[i] = Entry();
    }
    g_live = false;
}

bool audioDspIsOpen() { return g_live; }

int32_t audioDspMusicRegister(audio::SoundBank* bank, int32_t level) {
    char folder[kMusicPathBytes];
    snprintf(folder, sizeof(folder), "romfs:/music/level%d", (int)level);
    return audioDspMusicRegisterFolder(bank, folder);
}

int32_t audioDspMusicRegisterFolder(audio::SoundBank* bank, const char* folder) {
    // Cold (a level's load, the main menu's opening): the folder listed once,
    // each track's header read.
    snprintf(g_musicFolder, sizeof(g_musicFolder), "%s", folder);
    DIR* dir = opendir(g_musicFolder);
    if (dir == nullptr) {
        log3dsPrintf("music: %s is missing -- the level's music plays from the bank, unstreamed "
                     "and SILENT on the DSP", g_musicFolder);
        return 0;
    }
    int32_t count = 0;
    uint32_t bytes = 0;
    while (struct dirent* e = readdir(dir)) {
        const size_t len = strlen(e->d_name);
        if (len < 5 || len - 4 >= (size_t)kMusicNameBytes || strcmp(e->d_name + len - 4, ".dsp") != 0) {
            continue;
        }
        char path[kMusicPathBytes + kMusicNameBytes + 8];
        snprintf(path, sizeof(path), "%.63s/%.20s", g_musicFolder, e->d_name);
        FILE* f = fopen(path, "rb");
        if (f == nullptr) {
            continue;
        }
        uint8_t header[audio::DspAdpcm::kHeaderBytes];
        audio::DspAdpcmHeader head;
        const bool ok = fread(header, 1, sizeof(header), f) == sizeof(header) &&
                        audio::DspAdpcm::parseHeader(header, (int32_t)sizeof(header), &head);
        fseek(f, 0, SEEK_END);
        bytes += (uint32_t)ftell(f);
        fclose(f);
        if (!ok) {
            log3dsPrintf("music: %s is not a DSP-ADPCM track -- skipped", path);
            continue;
        }
        // The CMI's spelling is upper case; the file names are lower case.
        char name[kMusicNameBytes];
        for (size_t i = 0; i < len - 4; i++) {
            const char c = e->d_name[i];
            name[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
        }
        name[len - 4] = '\0';
        if (bank != nullptr) {
            bank->addStreamedMusic(name, (int32_t)head.sampleRate, (int32_t)head.samples);
        }
        count++;
    }
    closedir(dir);
    log3dsPrintf("music: %d DSP-ADPCM tracks in %s (%u bytes), streamed by the audio worker",
                 (int)count, g_musicFolder, (unsigned)bytes);
    return count;
}

void audioDspPrimeMusic(const std::string& name) {
    if (!g_live || name.empty()) {
        return;
    }
    LightLock_Lock(&g_lock);
    bool have = false;
    for (int32_t p = 0; p < kMusicPrimes; p++) {
        have = have || strcmp(g_primes[p].name, name.c_str()) == 0;
    }
    // Already being played on a channel: nothing to prime (LevelMusic does not
    // restart a track it is playing or fading).
    for (int32_t m = 0; m < VoiceBinder::kMusicChannels; m++) {
        have = have || (g_music[m].want && strcmp(g_music[m].name, name.c_str()) == 0);
    }
    if (!have) {
        // The older wish gives way (two primes: the loading arena's and the door
        // bubble's nearest).
        MusicPrime& prime = g_primes[g_primeNext];
        g_primeNext = (g_primeNext + 1) % kMusicPrimes;
        snprintf(prime.name, sizeof(prime.name), "%s", name.c_str());
    }
    LightLock_Unlock(&g_lock);
    if (!have) {
        kickWorker();
    }
}

void audioDspDetach() {
    if (!g_live) {
        return;
    }
    g_binder.reset();
    cutAll();
}

void audioDspFrame(const audio::SoundMixer& mixer) {
    if (!g_live) {
        return;
    }
    g_frame++;
    bool sounding[VoiceBinder::kChannels];
    for (int32_t c = 0; c < VoiceBinder::kChannels; c++) {
        sounding[c] = g_binder.channel(c).draining && ndspChnIsPlaying(ndspOf(c));
    }
    g_binder.sync(mixer.device(), sounding);

    for (int32_t c = 0; c < VoiceBinder::kEffectChannels; c++) {
        const VoiceBinder::Channel& ch = g_binder.channel(c);
        if (ch.start) {
            effectStart(c, ch, nameOf(mixer, ch.voice, ch.sound));
        } else if (ch.cut) {
            effectCut(c);
        } else if (ch.voice != 0 && ch.mixChanged) {
            setMix(c, ch.left, ch.right);
            ndspChnSetRate(c, ch.rate);
        }
    }
    for (int32_t m = 0; m < VoiceBinder::kMusicChannels; m++) {
        const VoiceBinder::Channel& ch = g_binder.channel(VoiceBinder::kFirstMusicChannel + m);
        if (ch.start) {
            musicStart(m, ch, nameOf(mixer, ch.voice, ch.sound));
        } else if (ch.cut) {
            musicStop(m, true);
        } else if (ch.voice == 0 && g_music[m].owned) {
            // Ended: stop feeding it and let the queued buffers play out.
            musicStop(m, false);
        } else if (ch.voice != 0 && ch.mixChanged) {
            musicMix(m, ch);
        }
    }
    musicSay();
}

AudioDspStats audioDspStats() {
    AudioDspStats st = {};
    const VoiceBinder::Stats& b = g_binder.stats();
    st.binds = b.binds;
    st.evictions = b.evictions;
    st.rebinds = b.rebinds;
    st.cuts = b.cuts;
    st.drains = b.drains;
    st.bound = g_binder.boundCount();
    st.cacheFills = g_cacheFills;
    st.cacheEvictions = g_cacheEvictions;
    st.cacheMisses = g_cacheMisses;
    st.arenaUsed = arenaUsed();
    st.arenaBytes = kSampleArenaBytes;
    if (g_live) {
        LightLock_Lock(&g_lock);
        st.musicQueued = g_musicQueued;
        st.musicRestarts = g_musicRestarts;
        st.musicUnprimed = g_musicUnprimed;
        st.musicMissing = g_musicMissing;
        st.musicReadMaxUs = g_musicReadMax;
        for (int32_t m = 0; m < VoiceBinder::kMusicChannels; m++) {
            st.musicActive += g_music[m].active ? 1 : 0;
        }
        LightLock_Unlock(&g_lock);
        st.dspFrames = ndspGetFrameCount();
        st.dspDropped = ndspGetDroppedFrames();
    }
    return st;
}

} // namespace mdk
