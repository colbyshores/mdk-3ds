// ============================================================================
// audio_dsp_3ds.cpp -- the DSP-mixed output (see audio_dsp_3ds.h).
// ============================================================================

#include "audio/audio_dsp_3ds.h"

#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <string>

#include "audio/voice_binding.h"
#include "platform/log_3ds.h"
#include "platform/vfp_fpscr.h"
#include "platform/worker_core.h"

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
// Forsaken's shape: four wave buffers per channel, refilled by a worker on a
// 25 ms tick. 4096 frames per buffer is 186 ms at 22050 Hz, so the queue holds
// 0.74 s -- 30 worker ticks of slack. Sized for stereo (every MDK track is mono,
// the store may not be): 4 x 4096 x 2 x 2 B = 64 KiB per channel, 128 KiB total.
constexpr int32_t kMusicBuffers = 4;
constexpr int32_t kMusicBufFrames = 4096;
constexpr int32_t kMusicBufHalfwords = kMusicBufFrames * 2;
constexpr uint32_t kMusicBytes =
    (uint32_t)VoiceBinder::kMusicChannels * kMusicBuffers * kMusicBufHalfwords * sizeof(int16_t);
constexpr s64 kWorkerTickNs = 25 * 1000 * 1000;
constexpr size_t kWorkerStackBytes = 16 * 1024;  // Forsaken's music worker's
constexpr float kMusicCutoff = 6000.0f;          // AudioDevice.MusicCutoff

struct MusicSlot {
    ndspWaveBuf wave[kMusicBuffers];
    int16_t* buf = nullptr;  // linear: kMusicBuffers x kMusicBufHalfwords
    // ---- guarded by g_lock (the worker reads them) ----
    bool active = false;     // the worker keeps this channel fed
    const audio::Sound* sound = nullptr;
    int32_t frames = 0;
    int32_t channels = 1;
    bool looping = false;
    int32_t cursor = 0;      // the next frame of the track to queue
    int32_t silentTicks = 0; // worker ticks in a row the channel was not playing
    float rate = 0.0f;
    float left = 0.0f;
    float right = 0.0f;
    // ---- main thread only ----
    bool owned = false;      // a logical voice is (or was, until stopped) streaming here
    std::string pin;         // the bank name this stream pins (empty: none)
};

bool g_live = false;
audio::SoundBank* g_bank = nullptr;
VoiceBinder g_binder;
EffectChannel g_effects[VoiceBinder::kEffectChannels];
MusicSlot g_music[VoiceBinder::kMusicChannels];
uint8_t* g_arena = nullptr;
int16_t* g_musicPool = nullptr;
Entry g_entries[kMaxEntries];
uint32_t g_useSerial = 0;
uint32_t g_frame = 0;
float g_lowPassE = 0.0f;  // exp(-2 pi fc / fs) at the DSP's rate

LightLock g_lock;
LightEvent g_wake;
Thread g_thread = nullptr;
bool g_exit = false;      // guarded by g_lock
bool g_suspended = false; // guarded by g_lock: the system is suspended / asleep
int32_t g_resumeGrace = 0;  // guarded by g_lock: ticks after a resume with no restart
aptHookCookie g_aptCookie;

uint32_t g_cacheFills = 0;
uint32_t g_cacheEvictions = 0;
uint32_t g_cacheMisses = 0;
uint32_t g_musicQueued = 0;   // guarded by g_lock
uint32_t g_musicRestarts = 0; // guarded by g_lock

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
    if (pcm8) {
        int8_t* out = (int8_t*)dst;
        for (size_t k = 0; k < count; k++) {
            out[k] = (int8_t)(src[k] / 256);  // exact: the value is a multiple of 256
        }
    } else {
        memcpy(dst, src, count * sizeof(int16_t));
    }
    DSP_FlushDataCache(dst, bytes);
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

// Reset the channel and configure it for the slot's stream. Under g_lock.
void musicConfigure(int32_t ndsp, const MusicSlot& slot) {
    ndspChnReset(ndsp);
    ndspChnSetInterp(ndsp, NDSP_INTERP_LINEAR);
    ndspChnSetFormat(ndsp, slot.channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
    ndspChnSetRate(ndsp, slot.rate);
    setMix(ndsp, slot.left, slot.right);
    // The music bus's one-pole low-pass (AudioDevice.AddMusic: y += (x - y) * a,
    // a = 1 - e), as a biquad in RBJ form with b1 = b2 = a2 = 0 (header).
    ndspChnIirBiquadSetParamsCustomFilter(ndsp, 1.0f, -g_lowPassE, 0.0f, 1.0f - g_lowPassE, 0.0f,
                                          0.0f);
    ndspChnIirBiquadSetEnable(ndsp, true);
}

// Queue wave buffer i with the next stretch of the track. Under g_lock. False
// when a one-shot track has nothing left.
bool musicFill(int32_t ndsp, MusicSlot& slot, int32_t i) {
    int16_t* dst = slot.buf + (size_t)i * kMusicBufHalfwords;
    const int16_t* src = slot.sound->samples.data();
    const int32_t ch = slot.channels;
    int32_t got = 0;
    // Each pass copies at least one frame or ends, so this is bounded by the
    // buffer's frame count.
    for (int32_t pass = 0; got < kMusicBufFrames && pass <= kMusicBufFrames; pass++) {
        if (slot.cursor >= slot.frames) {
            if (!slot.looping) {
                break;
            }
            slot.cursor = 0;
        }
        int32_t n = slot.frames - slot.cursor;
        if (n > kMusicBufFrames - got) {
            n = kMusicBufFrames - got;
        }
        memcpy(dst + (size_t)got * (size_t)ch, src + (size_t)slot.cursor * (size_t)ch,
               (size_t)n * (size_t)ch * sizeof(int16_t));
        got += n;
        slot.cursor += n;
    }
    if (got == 0) {
        return false;
    }
    DSP_FlushDataCache(dst, (u32)((size_t)got * (size_t)ch * sizeof(int16_t)));
    ndspWaveBuf& wb = slot.wave[i];
    memset(&wb, 0, sizeof(wb));
    wb.data_pcm16 = dst;
    wb.nsamples = (u32)got;
    wb.looping = false;
    wb.status = NDSP_WBUF_FREE;
    ndspChnWaveBufAdd(ndsp, &wb);
    g_musicQueued++;
    return true;
}

// The worker's tick for one music channel. Under g_lock.
void musicService(int32_t ndsp, MusicSlot& slot) {
    if (!slot.active) {
        return;
    }
    bool drained = true;
    for (int32_t i = 0; i < kMusicBuffers; i++) {
        const u8 st = slot.wave[i].status;
        drained = drained && (st == NDSP_WBUF_DONE || st == NDSP_WBUF_FREE);
    }
    slot.silentTicks = ndspChnIsPlaying(ndsp) ? 0 : slot.silentTicks + 1;
    if (drained && !slot.looping && slot.cursor >= slot.frames) {
        slot.active = false;  // the track played out: nothing to restart
        return;
    }
    // THE DRAINED-CHANNEL TRAP (Forsaken music_3ds.cpp, vault finding A): ndsp
    // does not resume a channel that ran dry when buffers are added to it, so it
    // is cleared, reset, re-configured and primed again. Two ways to be dry:
    // every buffer is back (nothing left to play), or the channel has not been
    // playing for two ticks in a row -- the last buffer finished between this
    // tick's status read and its refills, so the refills sit QUEUED on a dry
    // channel and never come back. NOT on one silent tick: a freshly primed
    // channel has QUEUED buffers before ndsp's next frame (~5 ms) marks it
    // playing, and restarting that would skip them. And not during a resume's
    // grace ticks, while ndsp may still be restoring its channels.
    if ((drained || slot.silentTicks >= 2) && g_resumeGrace == 0) {
        slot.silentTicks = 0;
        ndspChnWaveBufClear(ndsp);
        musicConfigure(ndsp, slot);
        g_musicRestarts++;
        for (int32_t i = 0; i < kMusicBuffers; i++) {
            slot.wave[i].status = NDSP_WBUF_FREE;
            musicFill(ndsp, slot, i);
        }
        return;
    }
    for (int32_t i = 0; i < kMusicBuffers; i++) {
        if (slot.wave[i].status == NDSP_WBUF_DONE) {
            musicFill(ndsp, slot, i);
        }
    }
}

// THE WORKER'S ENTRY, a plain function so the R1 gate (tools/runner/gates_vfp.py)
// resolves the routine threadCreate names and reads its body.
void audioDspWorker(void*) {
    // R1, AT THE WORKER'S ENTRY. Flush-to-zero and default-NaN are MDK's chosen
    // numerical mode, and FPSCR is per thread: the main thread's state is not
    // evidence of this one's. This worker does float work -- a drained channel's
    // restart sets its rate, its mix and its filter coefficients -- and decaying
    // values can reach subnormals in float code; modern libctru already
    // initialises the mode on its supported thread entry, so this is the
    // defensive, read-back form the contract asks every float thread to carry.
    const bool fzdn = vfpThreadFzDn();
    if (!fzdn) {
        log3dsPrintf("audio: dsp worker FZ/DN read-back FAILED");
    }
    for (;;) {
        LightEvent_WaitTimeout(&g_wake, kWorkerTickNs);
        LightLock_Lock(&g_lock);
        const bool exit = g_exit;
        // Suspended (HOME, sleep): ndsp has parked its channels; touching one
        // would fight its save and restore. The hook takes this lock, so a tick is
        // never half-way through a channel while the suspend proceeds.
        if (!exit && !g_suspended) {
            for (int32_t m = 0; m < VoiceBinder::kMusicChannels; m++) {
                musicService(kMusicNdsp[m], g_music[m]);
            }
            if (g_resumeGrace > 0) {
                g_resumeGrace--;
            }
        }
        LightLock_Unlock(&g_lock);
        if (exit) {
            break;
        }
    }
}

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

// Give back a music stream's bank pin (main thread: the bank is not thread-safe).
void musicUnpin(MusicSlot& slot) {
    if (!slot.pin.empty() && g_bank != nullptr) {
        g_bank->release(slot.pin);
    }
    slot.pin.clear();
}

// Stop feeding a music channel; `cut` also silences what is queued.
void musicStop(int32_t m, bool cut) {
    MusicSlot& slot = g_music[m];
    LightLock_Lock(&g_lock);
    slot.active = false;
    slot.sound = nullptr;
    if (cut) {
        ndspChnWaveBufClear(kMusicNdsp[m]);
    }
    LightLock_Unlock(&g_lock);
    slot.owned = false;
    musicUnpin(slot);
}

void musicStart(int32_t m, const VoiceBinder::Channel& order, const std::string& name) {
    MusicSlot& slot = g_music[m];
    const int32_t ndsp = kMusicNdsp[m];
    // The stream's own pin: the mixer releases its pin when the voice is
    // forgotten, which can be before this file stops reading the samples.
    if (g_bank != nullptr && !name.empty()) {
        g_bank->retain(name);
    }
    const std::string old = slot.pin;
    LightLock_Lock(&g_lock);
    ndspChnWaveBufClear(ndsp);
    slot.sound = order.sound;
    slot.channels = order.sound->channels == 2 ? 2 : 1;
    slot.frames = order.frames < order.sound->frames() ? order.frames : order.sound->frames();
    slot.looping = order.looping;
    slot.cursor = order.startFrame >= 0 && order.startFrame < slot.frames ? order.startFrame : 0;
    slot.silentTicks = 0;
    slot.rate = order.rate;
    slot.left = order.left;
    slot.right = order.right;
    slot.active = slot.frames > 0;
    if (slot.active) {
        musicConfigure(ndsp, slot);
        for (int32_t i = 0; i < kMusicBuffers; i++) {
            slot.wave[i].status = NDSP_WBUF_FREE;
            musicFill(ndsp, slot, i);
        }
    }
    LightLock_Unlock(&g_lock);
    slot.owned = true;
    slot.pin = name;
    if (!old.empty() && g_bank != nullptr) {
        g_bank->release(old);
    }
}

void musicMix(int32_t m, const VoiceBinder::Channel& order) {
    MusicSlot& slot = g_music[m];
    LightLock_Lock(&g_lock);
    slot.rate = order.rate;
    slot.left = order.left;
    slot.right = order.right;
    setMix(kMusicNdsp[m], order.left, order.right);
    ndspChnSetRate(kMusicNdsp[m], order.rate);
    LightLock_Unlock(&g_lock);
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

bool audioDspOpen(audio::SoundBank* bank) {
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
    g_musicPool = (int16_t*)linearAlloc(kMusicBytes);
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
        ndspExit();
        return false;
    }
    g_bank = bank;
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
        MusicSlot& slot = g_music[m];
        memset(slot.wave, 0, sizeof(slot.wave));
        slot.buf = g_musicPool + (size_t)m * kMusicBuffers * kMusicBufHalfwords;
        slot.active = false;
        slot.sound = nullptr;
        slot.owned = false;
        slot.pin.clear();
        ndspChnReset(kMusicNdsp[m]);
    }
    g_binder.reset();
    g_frame = 0;

    LightLock_Init(&g_lock);
    LightEvent_Init(&g_wake, RESET_ONESHOT);
    g_exit = false;
    g_suspended = false;
    g_resumeGrace = 0;
    aptHook(&g_aptCookie, onAptHook, nullptr);
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    // One ABOVE the main thread (a smaller number is a higher priority), which is
    // Forsaken's 0x28 against its 0x30 main -- and so above the async file reader
    // (main + 1), which shares core 1 with it: the 25 ms music refill preempts an
    // SD read, never waits behind one. On kWorkerCore (core 1, the owner decision
    // in worker_core.h, as t2k's audio does), off the main thread's core, so the
    // tick no longer interrupts a frame at all; if threadCreate refuses it, on the
    // process's default core as before, where the priority keeps it from waiting
    // for a busy frame to yield.
    const s32 workerPriority = priority > 0x18 ? priority - 1 : priority;
    int core = kWorkerCore;
    g_thread = threadCreate(audioDspWorker, nullptr, kWorkerStackBytes, workerPriority, core, false);
    if (g_thread == nullptr && core != kWorkerCoreFallback) {
        core = kWorkerCoreFallback;
        g_thread = threadCreate(audioDspWorker, nullptr, kWorkerStackBytes, workerPriority, core,
                                false);
    }
    g_live = true;
    log3dsPrintf("audio: dsp voices=%d music=ch%d+ch%d (crossfade) effects=ndsp 0-%d; arena %u B "
                 "linear (samples, PCM8 for 8-bit), music %d x %d x %d frames = %u B linear; "
                 "worker %s prio 0x%X stack %u core %d%s tick 25 ms; music low-pass biquad %.0f Hz "
                 "(e=%.6f at %.1f Hz); no limiter; linear free after %u B. The `audio: frame=` "
                 "sink ledger counts the software sink and reads 0 in this mode: read `audio: dsp`",
                 (int)VoiceBinder::kEffectChannels, (int)kMusicNdsp[0], (int)kMusicNdsp[1],
                 (int)VoiceBinder::kEffectChannels - 1, (unsigned)kSampleArenaBytes,
                 (int)VoiceBinder::kMusicChannels, (int)kMusicBuffers, (int)kMusicBufFrames,
                 (unsigned)kMusicBytes, g_thread ? "started" : "FAILED (music will stop)",
                 (unsigned)workerPriority, (unsigned)kWorkerStackBytes, core,
                 core != kWorkerCore ? " (kWorkerCore refused)" : "", (double)kMusicCutoff,
                 (double)g_lowPassE, (double)NDSP_SAMPLE_RATE, (unsigned)linearSpaceFree());
    return true;
}

void audioDspClose() {
    if (!g_live) {
        return;
    }
    aptUnhook(&g_aptCookie);
    // P4: signal, unblock, join -- then nothing the worker reads may go.
    if (g_thread != nullptr) {
        LightLock_Lock(&g_lock);
        g_exit = true;
        LightLock_Unlock(&g_lock);
        LightEvent_Signal(&g_wake);
        threadJoin(g_thread, U64_MAX);
        threadFree(g_thread);
        g_thread = nullptr;
    }
    cutAll();  // also gives back the bank pins (the bank outlives this call)
    g_binder.reset();
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
    g_bank = nullptr;
    g_live = false;
}

bool audioDspIsOpen() { return g_live; }

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
