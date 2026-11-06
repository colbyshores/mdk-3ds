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
//
// HOW TO READ THESE, AND THE ONE THING THEY CANNOT SHOW (review F1 of
// run-20261106-062909). `g_submitted++` and `g_inDsp++` happen together in
// submit(), and `g_done++` and `g_inDsp--` happen together at BOTH retirement
// sites, so `submitted - done == inDsp` is an identity and `inDsp <= kNumBufs`.
// So `done` is always within four of `submitted`: `done ~= submitted` is true of
// a completely broken sink, and `done ~= submitted/2` cannot happen at all. An
// earlier revision of this comment offered exactly that false reading, and the
// measured wrong-nsamples run falsified it — submitted=1030 done=1026, i.e.
// ~1.0, with dropped=1435. Do not read a rate out of `done / submitted`.
//
// THE IDENTITY IS AN OPEN-SINK IDENTITY (review F5 of run-20261106-062909).
// `audioClose()` zeroes `g_inDsp` and leaves `g_submitted` and `g_done` alone,
// so after a close the balance is whatever was still in flight (up to
// kNumBufs) against an `inDsp` of 0. Read the ledger only while
// `audioIsOpen()` is true; a closed sink's numbers are a transcript, not a
// state.
//
// The four readings that DO discriminate, each with the measured value from the
// run that produced it (all ~43 s of guest time, level 7):
//
//   submitted against what the CLOCK owes, not what the frame count owes.
//     Production is real time by construction: the device mixes rate*delta per
//     frame and `GameFlow::elapsedFor(false, step, delta)` hands it the WALL
//     delta (main_3ds.cpp:449), so over T seconds the device owes
//     T x 44100/882 = T x 50 buffers whatever the frame rate did. The
//     N x 735/882 form of this test is the SAME number only at exactly 60 fps,
//     because 735 is one 1/60 s frame at 44100 Hz (review F3). At 30 fps the
//     same wall-clock period owes twice as many buffers, so a sink producing
//     half the right rate scores 1.00 against a frame-count denominator: the
//     fps precondition is part of the test, not a footnote to it. Measured at
//     60 fps, where 2580 frames are 43 s and owe ~2150: correct build 2156
//     (ratio 1.00). Wrong nsamples: 1030 at 2460 (0.50) — the DSP held each
//     buffer twice as long as its bytes are worth, so half the fills found a
//     full queue and were never submitted. Nothing ever queued at all: 4 (0.00).
//     THIS is the rate test, and it is why the ledger line carries the frame.
//   inDsp pinned at kNumBufs while dropped climbs: NOTHING HAS RETIRED, which
//     does not by itself say why (review F4). The pre-`status = FREE` run read
//     inDsp=4 dropped=2579 with submitted=4 done=0 — ndsp had accepted nothing,
//     so the four buffers sat there forever — and a merely slow DSP drains the
//     same four buffers late and reads the same way. What separates "the queue
//     is jammed" from "ndsp never took a buffer" is `done` and dspframes
//     advancing, not inDsp. Both broken runs read inDsp=4.
//   requeued ~= submitted: the channel drained on every submit, i.e. consumption
//     is FASTER than production. One requeue at level load is normal (measured
//     requeued=1).
//   dspframes advancing ~205 per 60 guest frames: the DSP is running.
//     THE UNIT, read off the disassembly of libctru.a's ndsp.o (the same
//     instrument as the `ndspChnWaveBufAdd` reading below): `.bss.frameCount`
//     is zeroed by `ndspInitialize` (`str r1, [r2]` at +0x13c with r1 = 0) and
//     incremented by exactly one `add r3, r3, #1` per iteration of
//     `ndspThreadMain` (+0x1a8..+0x1b4), immediately after the `svcSignalEvent`
//     that hands the frame to the DSP, and `ndspGetFrameCount` is its only
//     reader. So it counts ndsp MIXER frames — not samples, not our wave
//     buffers, and not this channel's 44100 Hz, which is resampled into the
//     DSP's own rate.
//     WHAT THE DISASSEMBLY PROVES IS THE KIND, NOT THE RATE, and the rate is
//     the half a reader wants (review-8 F1). `ndsp.h:9` is
//     `#define NDSP_SAMPLE_RATE (SYSCLOCK_SOC / 512.0)` and `os.h:9` is
//     `#define SYSCLOCK_SOC (16756991u)`, so the DSP rate is 16756991/512 =
//     32728.5 Hz — NOT a power of two, and batch 88 rounded it to 32768 for
//     precisely the reason this paragraph gives. The frame
//     QUANTUM is not named by anything installed (it lives in the DSP firmware,
//     and /opt/devkitpro/libctru ships headers and a library, no source), so
//     160 samples starts as an assumption and 32728.5/160 = 204.553/s is a
//     prediction.
//     THE PREDICTION NOW HAS A MEASUREMENT, AND WHAT WAS WRONG WAS THE RATIO,
//     NOT THE MODEL (review-10 F1). Three batches divided one row's dpf by that
//     row's `secs` and then explained the leftover as run noise. It is not
//     noise — and it is NOT as reproducible as batch 91 claimed either. THREE runs
//     (one on the parent ELF with `secs` at 2 dp, two on this build at 3 dp) agree
//     on `secs` to the millisecond at every frame number, because `secs` is a sum
//     of frame deltas, and agree on `dspframes` only to within ONE count: 23 of 48
//     rows match exactly and 25 read one lower (228 against 229 at frame 60, 6587
//     against 6588 at frame 1920, and all three back together at 9870 by frame
//     2880). So the instrument is reproducible to a frame, not frame for frame
//     (review-11 R10-4 asked for an attribution fix on "two runs of this build";
//     the re-measurement that checked it found a bigger error in the same
//     sentence), and batch 90's "0.078/s run-to-run spread" was one trajectory
//     sampled twice plus a one-count phase jitter, never a rate difference.
//     dpf/secs from a single row carries a START OFFSET, because the counter
//     starts when ndsp starts and `secs` starts when the first frame is
//     measured. Divide by the header model and subtract it: dpf/204.553 - secs
//     stays inside +0.014..+0.028 s across three 47 s runs and drifts by
//     -0.002..+0.003 s (about one 20 ms buffer; the runs bound the offset, they do
//     not pin where it comes from), while under a power-of-two 204.80 the same
//     subtraction drifts by -0.054..-0.059 s OVERALL — not monotonically: it rises
//     on 9 of the 47 row-to-row steps of one run (largest rise +0.004 s) and on 32
//     of 47 of another (largest +0.006 s), which is the same row-to-row jitter as
//     the +/-0.05/s the sub-windows show, integrated over a 47 s baseline. The trend
//     is the finding, not the strictness of the descent (review-11 R10-3). A
//     constant offset cancels in a SLOPE
//     between two rows, so the slope is the estimator and the ratio never was:
//     (9870-229)/(48.233-1.099) = 204.544/s and (9870-228)/(48.233-1.099) =
//     204.566/s on two runs of this build, 204.562/s on the parent ELF. That
//     0.021/s spread IS the estimator's own quantum, +/-1 count over 47.134 s, and
//     it is the honest error bar on every number in this paragraph. Against the
//     header model's 204.553 the three read -0.009, +0.013 and +0.009/s (-0.004 %
//     to +0.006 %) and against a power-of-two 204.800 they read -0.256, -0.234 and
//     -0.238/s (-0.115 % to -0.125 %), so the model that survives is the header's
//     by an order of magnitude more than the spread. The implied quantum is
//     160.007, 160.001 and 159.990 samples. The nearest OTHER integer
//     quantum is 161, which would run at 32728.5/161 = 203.283/s, 1.26/s below the
//     measurement — 25x the scatter — and the measurement sits 0.63/s from the
//     halfway point where the implied quantum would tip (160.5 => 203.916/s), so
//     the quantum is 160 or it is nothing (review-11 R10-5: an earlier draft said
//     "the next integer is 0.6/s away", which is that half-step tipping distance
//     mislabelled as a distance to an integer).
//     DO NOT READ THE ROUNDING BAND AS THE UNCERTAINTY. At 3 dp the arithmetic
//     band on that slope is +/-0.004/s, which would put 204.553 OUTSIDE it — but
//     four 11 s sub-windows of the SAME run read 204.533, 204.605, 204.515 and
//     204.605, a real scatter of +/-0.05/s that has nothing to do with the
//     print's decimals: `secs` is the guest's frame sum, dpf is the HLE mixer's
//     own counter, and the two drift against each other at the hundredths of a
//     percent level over tens of seconds. Against THAT scatter the header model
//     fits (0.009/s, a fifth of it) and a power-of-two does not (0.256/s, five
//     times it). That is the whole of what this instrument can settle, and
//     quoting the rounding band instead would be the same mistake as the 2-dp
//     exclusion batch 89 made, only pointing the other way.
//     WHAT IS STILL NOT MEASURED IS THE CONSOLE. This is Mandarine's HLE DSP, so
//     204.544/s is a property of the emulator's model of the mixer: strong
//     evidence for a 160-sample frame at 32728.5 Hz, not a header for it, and
//     the Old-3DS mixer rate stays UNMEASURED (AGENTS P6/P8). The quantum also
//     stays unnamed by anything installed — the run confirms the emulator agrees
//     with 160, it does not make 160 a documented constant. So on hardware the
//     ledger line is still read as a liveness signal first, and only turned into
//     a rate against a console-side clock. `secs` is printed to 3 dp because the
//     2-dp print put +/-0.043/s on every slope, which is the size of the scatter
//     itself: the instrument could not tell its own rounding error from the
//     clock pair's, and that is how a fixed 20 ms offset came to be read as
//     noise.
//     An earlier revision of this comment called the unit "240-sample blocks at
//     48 kHz", which is wrong twice over and was never checked (review F2), and
//     it survived because 48000/240 = 200/s fits the same measurement. A unit
//     that only has to be right in magnitude cannot be falsified by a magnitude
//     — which is the lesson this paragraph is now proof of three times.
//
// `dropped` counts REJECTED FILLS, not buffers: one increment can throw away a
// whole frame's worth of samples (up to 735 frames, 0.83 of a buffer), so it is
// not comparable with submitted/2 and it is not a buffer count.
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
    //   +0x48: ldrb ip, [r1, #17] ; sub ip, #1 ; cmp ip, #1 ; bls +0xb8
    //   +0x60: mov r3, #1 ; strb r3, [r1, #17] ; str r3(zero), [r1, #20]
    //   ...append to the channel's list...
    //   +0xac: strh r3, [r1, #18]        ; sequence_id
    //   +0xb8: add sp, sp, #20 ; pop {lr} ; b LightLock_Unlock   ; the epilogue
    //
    // i.e. for status QUEUED(1) or PLAYING(2) the branch at +0x54 goes straight
    // to the epilogue at +0xb8: it links NOTHING and assigns no sequence id, the
    // strh at +0xac being reachable only by falling through from +0x58 on the
    // linking path. It is libctru that sets QUEUED on the way in. Pre-setting QUEUED — which is what this line used to do — made
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
