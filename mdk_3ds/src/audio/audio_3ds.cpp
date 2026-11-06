// ============================================================================
// audio_3ds.cpp — the 3DS sink (SPEC.md 8's ndsp half of the audio table).
// ============================================================================

#include "audio/audio_3ds.h"

#include <3ds.h>
#include <string.h>
#include <unistd.h>

#include "platform/log_3ds.h"
#include "platform/vfp_fpscr.h"

namespace mdk {
namespace {

// The output rate is the shared device's (SoundDevice::kOutputRate); the sink
// does not choose it, it matches it.
constexpr int kRate = 44100;
constexpr int kChannels = 2;
// 20 ms of stereo, four of them: 14,112 bytes of linear memory. See the header
// for why four and not two.
constexpr int kBufFrames = kRate / 50;               // 882
constexpr int kBufHalfwords = kBufFrames * kChannels;  // per buffer
constexpr int kNumBufs = 4;
// The ndsp channel that carries the mix. One, on purpose (header).
constexpr int kChannel = 0;

struct Slot {
    ndspWaveBuf wave;
    int16_t* linear;
    int32_t filled;  // halfwords written since the buffer was taken
    bool inDsp;      // queued with ndsp and not yet reaped
};

Slot g_slots[kNumBufs];
uint8_t* g_pool = nullptr;
int g_write = 0;      // the slot being filled now
int g_inDsp = 0;      // buffers ndsp still holds
bool g_live = false;

// The observability the batch that landed this did not have (review F4).
// Production is real time by construction — the device mixes rate*delta frames
// per frame — so `submitted` already says how many seconds of audio the mix made
// (submitted x 20 ms) and `done` says how many the DSP gave back in that same
// time. done ~= submitted means the DSP consumes at the rate the nameplate says;
// done ~= submitted/2 with dropped ~= submitted/2 means it is taking twice as
// long per buffer as the bytes are worth, which is exactly what a wrong nsamples
// unit does. Without these, every one of those states looked like a working sink.
uint32_t g_submitted = 0;
uint32_t g_done = 0;
uint32_t g_dropped = 0;
uint32_t g_requeued = 0;

// The channel's configuration, in one place because the underrun path has to
// apply it again. ndsp does NOT resume a drained channel when a buffer is added
// to it — the same trap Forsaken hit with its music queue
// (vault: 10-projects/forsaken/findings/dsp-adpcm-music-and-sfx-audio.md §A,
// "the broken record fix" section's loop kickstart) — so a drained channel is
// cleared, reset and re-configured before the next buffer goes in.
void configureChannel() {
    ndspChnReset(kChannel);
    ndspChnSetFormat(kChannel, NDSP_FORMAT_STEREO_PCM16);
    ndspChnSetInterp(kChannel, NDSP_INTERP_LINEAR);
    ndspChnSetRate(kChannel, (float)kRate);
}

// ndsp marks a finished buffer DONE; nothing else tells us, so the reap is a
// poll of our own slots.
void reap() {
    for (int i = 0; i < kNumBufs; i++) {
        Slot& s = g_slots[i];
        if (s.inDsp && s.wave.status == NDSP_WBUF_DONE) {
            s.inDsp = false;
            s.filled = 0;
            g_inDsp--;
            g_done++;
        }
    }
}

void submit(Slot& s) {
    if (g_inDsp == 0) {
        // The channel had drained. A sink that re-configures on every submit is
        // consuming faster than it produces, which is the other wrong-nsamples
        // signature (see the counters above).
        g_requeued++;
        configureChannel();  // the channel drained: it will not wake on its own
    }
    DSP_FlushDataCache(s.linear, (u32)kBufHalfwords * sizeof(int16_t));
    s.wave.data_pcm16 = s.linear;
    // nsamples IS IN FRAMES, NOT HALFWORDS, for a stereo format — measured, not
    // argued (review F1). libctru's header comment says "PCM16=halfwords" and
    // one devkitPro example (ogg-vorbis-decoding) follows it, but four others
    // that are unambiguously STEREO pass frames (audio/streaming and
    // audio/filters: nsamples = SAMPLESPERBUF with a 4-bytes-per-frame buffer;
    // audio/modplug-decoding: (decoded/2)/sizeof(int16_t); audio/opus-decoding:
    // totalSamples counted in frames), and so does the hardware-proven
    // ../forsaken/sound_3ds.c:467 that the owner directive told this port to
    // adapt. The measurement settles it: with nsamples = kBufHalfwords the DSP
    // retired 25 buffers per 60 guest frames while the mix produced 50 — exactly
    // half, i.e. it spent 40 ms on a buffer holding 20 ms of stereo, which is
    // what reading 1764 as 1764 FRAMES does. With the frames value the queue
    // matches production (see the batch's verdict block). The halfwords reading
    // was also a 3528-byte DSP read past a 3528-byte slot, and past the end of
    // the linearAlloc for the last slot.
    s.wave.nsamples = (u32)kBufFrames;
    s.wave.looping = false;
    // STATUS MUST BE FREE, NOT QUEUED. ndspChnWaveBufAdd does not queue a buffer
    // it believes is already queued: disassembly of libctru's ndsp-channel.o
    // (r1 = the wavebuf, offset 17 = status) reads
    //
    //   ldrb ip, [r1, #17] ; sub ip, #1 ; cmp ip, #1 ; bls <just assign sequence_id>
    //   mov r3, #1 ; strb r3, [r1, #17] ; str r3(zero), [r1, #20]  ; status=QUEUED, next=0
    //   ...append to the channel's list...
    //
    // i.e. for status QUEUED(1) or PLAYING(2) it assigns a sequence id and
    // returns WITHOUT linking the buffer, and it is libctru that sets QUEUED on
    // the way in. Pre-setting QUEUED — which is what this line used to do — made
    // every submit a no-op: the DSP was handed nothing, so nothing ever came
    // back DONE, so the queue filled after four buffers and the sink dropped
    // every mix after the first 80 ms. The counters caught it (submitted=4
    // done=0 dropped=2579 at frame 2580 in the run before this comment was
    // written) after the batch that shipped it had reported success, because
    // `plays=1` counts launched mixer VOICES and says nothing about ndsp
    // (review F4).
    s.wave.status = NDSP_WBUF_FREE;
    ndspChnWaveBufAdd(kChannel, &s.wave);
    s.inDsp = true;
    g_inDsp++;
    g_submitted++;
    s.filled = 0;
}

// The next slot that ndsp has finished with, or -1 when every buffer is still in
// the DSP's hands (an over-full queue: the caller drops the frames).
int freeSlot() {
    for (int n = 0; n < kNumBufs; n++) {
        const int i = (g_write + n) % kNumBufs;
        if (!g_slots[i].inDsp || g_slots[i].wave.status == NDSP_WBUF_DONE) {
            return i;
        }
    }
    return -1;
}

} // namespace

bool audioOpen() {
    // R1, THE AUDIO ROW. The contract's R1 table names this file as the audio
    // thread's entry, and the helper is called here -- but THE DESIGN HAS NO AUDIO
    // WORKER: the mix is produced on the frame's own thread, because the sink has
    // nothing to read and no blocking work to hide from the frame (see the
    // header). That thread is main, which established FZ|DN at the top of main()
    // before gfxInitDefault. So this call is defensive and idempotent -- at most
    // one vmsr, once per process -- and it is here for two reasons. R1 is
    // per-thread, and the numbers it protects are the denormal-range ones: the
    // mix's accumulating sums and the sink's float-to-PCM16 scaling are exactly
    // the arithmetic whose behaviour changes when FZ and DN are not the chosen
    // mode, and R1 asks for a read-back rather than an assumption. And if a later
    // batch moves the fill to a worker, the helper is already at the audio
    // subsystem's entry instead of somewhere that merely looks like one. The
    // read-back is logged.
    const bool fzdn = mdk::vfpThreadFzDn();
    mdk::log3dsPrintf("audio: FZ/DN %d (fpscr=%08X)", fzdn ? 1 : 0,
                      (unsigned)mdk::vfpReadFpscr());
    if (g_live) {
        return true;
    }
    // The DSP firmware is not redistributable, so it is not in the romfs; the
    // user's CFW put it at sdmc:/3ds/dspfirm.cdc. Missing, ndspInit returns
    // 0xD880A7FA and the game is silent — so say so in the log instead of
    // leaving silence as the only message.
    if (access("sdmc:/3ds/dspfirm.cdc", F_OK) != 0) {
        mdk::log3dsPrintf("audio: sdmc:/3ds/dspfirm.cdc is missing — running muted");
        return false;
    }
    const Result rc = ndspInit();
    if (rc != 0) {
        mdk::log3dsPrintf("audio: ndspInit failed %08X — running muted", (unsigned)rc);
        return false;
    }
    ndspSetMasterVol(1.0f);

    g_pool = (uint8_t*)linearAlloc(sizeof(int16_t) * kBufHalfwords * kNumBufs);
    if (g_pool == nullptr) {
        mdk::log3dsPrintf("audio: linearAlloc(%u) failed — running muted",
                          (unsigned)(sizeof(int16_t) * kBufHalfwords * kNumBufs));
        ndspExit();
        return false;
    }
    memset(g_slots, 0, sizeof(g_slots));
    for (int i = 0; i < kNumBufs; i++) {
        g_slots[i].linear = (int16_t*)(g_pool + (size_t)i * kBufHalfwords * sizeof(int16_t));
        g_slots[i].filled = 0;
        g_slots[i].inDsp = false;
    }
    configureChannel();
    g_write = 0;
    g_inDsp = 0;
    g_live = true;
    mdk::log3dsPrintf("audio: ndsp chn%d PCM16 stereo %d Hz, %d x %d ms queued", kChannel, kRate,
                      kNumBufs, 1000 / 50);
    return true;
}

void audioClose() {
    if (!g_live) {
        return;
    }
    ndspChnWaveBufClear(kChannel);
    ndspExit();
    if (g_pool != nullptr) {
        linearFree(g_pool);
        g_pool = nullptr;
    }
    for (int i = 0; i < kNumBufs; i++) {
        g_slots[i].linear = nullptr;
        g_slots[i].inDsp = false;
        g_slots[i].filled = 0;
    }
    g_inDsp = 0;
    g_live = false;
}

bool audioIsOpen() { return g_live; }

AudioStats audioStats() {
    AudioStats st = {};
    st.submitted = g_submitted;
    st.done = g_done;
    st.dropped = g_dropped;
    st.requeued = g_requeued;
    st.inDsp = g_inDsp;
    // ndsp's own counters, so "the DSP is running" is not inferred from our
    // bookkeeping. Zero when ndsp never started.
    st.dspFrames = g_live ? ndspGetFrameCount() : 0u;
    st.dspDropped = g_live ? ndspGetDroppedFrames() : 0u;
    return st;
}

// The sink: interleaved stereo float, full scale 1.0, into PCM16 wave buffers.
//
// THE CONVERSION IS THE SPEC's, not an invention: SPEC.md 8 puts it here ("the
// shared mixer's float output is converted at the sink") precisely so the mix
// itself stays the shared, oracle-pinned float one. Rounding is half-away-from-
// zero at 32767 full scale, and the clamp is defensive: the limiter already
// bounds the mix at its -0.5 dB ceiling, so nothing should reach it, and a
// wrap-around on a value that somehow does would be a loud pop instead of a
// flat-top.
//
// HOT PATH by AGENTS.md 2's second criterion: a 1/60 s frame is 735 frames x 2
// channels = 1470 conversions, over 256, so the loop carries its pin. The
// conversion is the sink's output format and has no closed form without it; the
// clamp's two compares are the same arithmetic the limiter's own clamp uses.
void audioSink(void* /*context*/, const float* interleaved, int32_t frames) {
    if (!g_live || interleaved == nullptr || frames <= 0) {
        return;
    }
    reap();

    int32_t left = frames * kChannels;
    const float* in = interleaved;
    while (left > 0) {
        Slot& slot = g_slots[g_write];
        if (slot.inDsp) {
            if (slot.wave.status != NDSP_WBUF_DONE) {
                const int next = freeSlot();
                if (next < 0) {
                    g_dropped++;
                    return;  // the queue is full: drop, never block the frame
                }
                g_write = next;
                continue;
            }
            // Retired between reap() and here. Counted, or the ledger would
            // quietly under-report done and the rate it exists to show would be
            // wrong by however often this path runs.
            slot.inDsp = false;
            slot.filled = 0;
            g_inDsp--;
            g_done++;
        }
        const int32_t room = kBufHalfwords - slot.filled;
        const int32_t n = left < room ? left : room;

        int16_t* out = slot.linear + slot.filled;
        /* @vfp-exempt R2,R3 — the sink's float->PCM16 conversion: the (int)
         * conversion that produces the sample (R2) and the two compares that
         * clamp it (R3). The output format is what ndsp accepts, so there is no
         * float-free form of this loop; the limiter already bounds the mix at
         * its ceiling, so the clamp is defensive and not a range search. All
         * float, no double (R4). measured n/a (corpus empty). Verified
         * 2026-11-06. */
        for (int32_t i = 0; i < n; i++) {
            float v = in[i] * 32767.0f;
            if (v > 32767.0f) {
                v = 32767.0f;
            } else if (v < -32768.0f) {
                v = -32768.0f;
            }
            out[i] = (int16_t)(v + (v >= 0.0f ? 0.5f : -0.5f));
        }
        slot.filled += n;
        in += n;
        left -= n;
        if (slot.filled >= kBufHalfwords) {
            submit(slot);
            g_write = (g_write + 1) % kNumBufs;
        }
    }
}

} // namespace mdk
