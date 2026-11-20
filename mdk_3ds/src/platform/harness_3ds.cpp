// ============================================================================
// harness_3ds.cpp -- see harness_3ds.h. The whole file is the harness
// build's: without MDK_AUTOQUIT it compiles to the two no-op answers, so
// the release binary carries no trace of the cycle (no deadline, no
// chain-load, no hb:ldr traffic).
// ============================================================================

#include "platform/harness_3ds.h"

#ifdef MDK_AUTOQUIT

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "platform/log_3ds.h"

namespace mdk {

bool harnessAutoquitDue() {
    static u64 quitAtTick = 0;
    const u64 now = svcGetSystemTick();
    if (quitAtTick == 0) {
        // Armed on the first frame, not at init (the runbook: an init-time
        // arm measures the boot, not the game).
        quitAtTick = now + (u64)(MDK_AUTOQUIT) * (u64)SYSCLOCK_ARM11;
        return false;
    }
    return now >= quitAtTick;
}

#ifdef MDK_HW_PROFILE
namespace {
// 0.1 ms buckets to 100 ms, one overflow bucket. Frame periods on this
// target sit at 16.7/33.3/50 ms (vsync-quantized), so 0.1 ms resolves the
// quantum and its jitter; anything over 100 ms is a stall, counted, with
// its true maximum kept separately.
constexpr uint32_t kBucketUs = 100;
constexpr uint32_t kBuckets = 1000;
uint32_t g_hist[kBuckets + 1];
uint32_t g_count = 0;
uint64_t g_sumUs = 0;
uint32_t g_maxUs = 0;
uint32_t g_maxAt = 0;    // the profiled frame (1-based) that took g_maxUs
// THE STEADY STATE. The frame period is quantised by the 60 Hz refresh (a
// frame takes 2 or 3 vblanks: 33.3 or 50 ms), so the p50 flips between those
// with no real change (measured 2026-11-19: 35.1 against 49.5 ms for builds
// whose means differ by 1.2 ms); and the level's first frames carry the
// first-use work (seconds). So the summary also gives the mean from the third
// frame on, and how many frames took 1, 2, 3 and 4+ vblanks.
constexpr uint32_t kSteadyFrom = 3;
uint64_t g_steadySumUs = 0;
uint32_t g_steadyCount = 0;
uint32_t g_vblanks[5] = {0, 0, 0, 0, 0};   // [1], [2], [3], [4+]; [0] unused
// THE SPIKES, NAMED. A frame over kSpikeUs (60 ms: four vblanks or more) is
// logged with the previous frame's spans -- the spans of frame N are reported
// at its end, its period measured at the start of N+1 -- so a spike says what
// it was made of (simulation steps, the draw build, the GPU waits). The first
// kSpikeLogs only: the log is an SD write.
constexpr uint32_t kSpikeUs = 60000u;
constexpr uint32_t kSpikeLogs = 40u;
uint32_t g_spikesLogged = 0;
uint64_t g_lastSimT = 0, g_lastBeginT = 0, g_lastDrawT = 0, g_lastEndT = 0, g_lastAudioT = 0;
uint32_t g_lastSteps = 0;
uint32_t g_over17 = 0;   // > 16.667 ms: missed a 60 Hz vblank
uint32_t g_over34 = 0;   // > 33.333 ms: missed a 30 Hz vblank
bool g_stereoSeen = false;
uint64_t g_simT = 0, g_beginT = 0, g_drawT = 0, g_endT = 0, g_audioT = 0;
uint64_t g_simSteps = 0;
uint32_t g_spanFrames = 0;
uint64_t g_playerT = 0, g_runtimeT = 0, g_cameraT = 0, g_solidT = 0;

uint32_t percentileUs(uint32_t pct) {
    // The bucket's upper edge: an estimate, stated as one in the log line.
    const uint32_t want = (uint32_t)(((uint64_t)g_count * pct + 99) / 100);
    uint32_t seen = 0;
    for (uint32_t b = 0; b <= kBuckets; b++) {
        seen += g_hist[b];
        if (seen >= want && want > 0) {
            return b == kBuckets ? g_maxUs : (b + 1) * kBucketUs;
        }
    }
    return g_maxUs;
}
} // namespace
#endif

void harnessProfileFrame(double periodSeconds) {
#ifdef MDK_HW_PROFILE
    // Once per frame, outside any loop over the frame's work: one double
    // compare and one conversion, never in a hot loop (AGENTS.md R2 scope).
    if (periodSeconds < 0.0) periodSeconds = 0.0;
    if (periodSeconds > 4000.0) periodSeconds = 4000.0;
    const uint32_t us = (uint32_t)(periodSeconds * 1000000.0);
    // A literal divisor (kBucketUs), so the compiler emits a multiply rather
    // than __aeabi_uidiv (AGENTS.md R11; this runs once per frame).
    const uint32_t b = us / 100u;
    static_assert(kBucketUs == 100u, "the bucket divisor above is kBucketUs");
    g_hist[b < kBuckets ? b : kBuckets]++;
    g_count++;
    g_sumUs += us;
    if (us > g_maxUs) {
        g_maxUs = us;
        g_maxAt = g_count;
    }
    // A STALL is named when it happens (frame number, length), not only folded
    // into the summary's max: the summary cannot say which frame, and the frame
    // log's `elapsed` is the game's clamped step, not the wall clock. Rare by
    // definition, so the log write is not a per-frame cost.
    if (us > kSpikeUs && us <= 500000u && g_count >= kSteadyFrom && g_spikesLogged < kSpikeLogs) {
        g_spikesLogged++;
        const double usPerTick = 1000000.0 / (double)SYSCLOCK_ARM11;
        mdk::log3dsPrintf("profile: spike %lu.%01lu ms at profiled frame %lu: sim %.2f ms (%lu steps) "
                          "begin %.2f draw %.2f end %.2f audio %.2f",
                          (unsigned long)(us / 1000u), (unsigned long)(us % 1000u / 100u),
                          (unsigned long)g_count, (double)g_lastSimT * usPerTick / 1000.0,
                          (unsigned long)g_lastSteps, (double)g_lastBeginT * usPerTick / 1000.0,
                          (double)g_lastDrawT * usPerTick / 1000.0,
                          (double)g_lastEndT * usPerTick / 1000.0,
                          (double)g_lastAudioT * usPerTick / 1000.0);
    }
    if (us > 500000u) {
        mdk::log3dsPrintf("profile: STALL %lu.%03lu ms at profiled frame %lu",
                          (unsigned long)(us / 1000u), (unsigned long)(us % 1000u),
                          (unsigned long)g_count);
    }
    if (g_count >= kSteadyFrom) {
        g_steadySumUs += us;
        g_steadyCount++;
        // Nearest whole vblank (16.667 ms), by integer arithmetic: the divisor
        // is a literal (AGENTS.md R11).
        const uint32_t vb = (us + 8333u) / 16667u;
        g_vblanks[vb < 1u ? 1u : (vb > 4u ? 4u : vb)]++;
    }
    if (us > 16667) g_over17++;
    if (us > 33333) g_over34++;
    if (osGet3DSliderState() > 0.0f) g_stereoSeen = true;
#else
    (void)periodSeconds;
#endif
}

uint64_t harnessProfileTick() {
#ifdef MDK_HW_PROFILE
    return svcGetSystemTick();
#else
    return 0;
#endif
}

void harnessProfileStepSpans(uint64_t playerTicks, uint64_t runtimeTicks,
                             uint64_t cameraTicks, uint64_t solidTicks) {
#ifdef MDK_HW_PROFILE
    g_playerT += playerTicks;
    g_runtimeT += runtimeTicks;
    g_cameraT += cameraTicks;
    g_solidT += solidTicks;
#else
    (void)playerTicks; (void)runtimeTicks; (void)cameraTicks; (void)solidTicks;
#endif
}

void harnessProfileSpans(uint64_t simTicks, uint32_t simSteps, uint64_t beginTicks,
                         uint64_t drawTicks, uint64_t endTicks, uint64_t audioTicks) {
#ifdef MDK_HW_PROFILE
    g_simT += simTicks;
    g_simSteps += simSteps;
    g_beginT += beginTicks;
    g_drawT += drawTicks;
    g_endT += endTicks;
    g_audioT += audioTicks;
    g_spanFrames++;
    g_lastSimT = simTicks;
    g_lastSteps = simSteps;
    g_lastBeginT = beginTicks;
    g_lastDrawT = drawTicks;
    g_lastEndT = endTicks;
    g_lastAudioT = audioTicks;
#else
    (void)simTicks; (void)simSteps; (void)beginTicks; (void)drawTicks; (void)endTicks;
    (void)audioTicks;
#endif
}

void harnessProfileReport() {
#ifdef MDK_HW_PROFILE
    if (g_spanFrames > 0) {
        // Ticks -> microseconds once, here (the runbook's rule: raw ticks in the
        // bracket, convert once per report). Per frame, and per step for sim.
        const double usPerTick = 1000000.0 / (double)SYSCLOCK_ARM11;
        const double f = (double)g_spanFrames;
        const double simUs = (double)g_simT * usPerTick;
        mdk::log3dsPrintf("profile: spans frames=%lu sim=%.2fms/frame steps=%.2f/frame "
                          "step=%.2fms begin(vblank wait)=%.2fms draw=%.2fms end=%.2fms "
                          "audio=%.2fms",
                          (unsigned long)g_spanFrames, simUs / f / 1000.0,
                          (double)g_simSteps / f,
                          g_simSteps ? simUs / (double)g_simSteps / 1000.0 : 0.0,
                          (double)g_beginT * usPerTick / f / 1000.0,
                          (double)g_drawT * usPerTick / f / 1000.0,
                          (double)g_endT * usPerTick / f / 1000.0,
                          (double)g_audioT * usPerTick / f / 1000.0);
        // The step's parts, per STEP, and the residual so the accounting is
        // seen to close (the runbook's rule: an unexplained residual is where
        // the next phantom hides).
        const double steps = g_simSteps ? (double)g_simSteps : 1.0;
        const double parts = (double)(g_playerT + g_runtimeT + g_cameraT + g_solidT);
        mdk::log3dsPrintf("profile: step player=%.2fms runtime=%.2fms camera=%.2fms "
                          "solid=%.2fms residual=%.2fms (per step)",
                          (double)g_playerT * usPerTick / steps / 1000.0,
                          (double)g_runtimeT * usPerTick / steps / 1000.0,
                          (double)g_cameraT * usPerTick / steps / 1000.0,
                          (double)g_solidT * usPerTick / steps / 1000.0,
                          ((double)g_simT - parts) * usPerTick / steps / 1000.0);
    }
    u8 model = 0xFF;
    if (R_SUCCEEDED(cfguInit())) {
        CFGU_GetSystemModel(&model);
        cfguExit();
    }
    // 0 = 3DS, 1 = 3DS XL, 3 = 2DS: the Old-3DS family, 268 MHz, no L2.
    // 2 = New 3DS, 4 = New 3DS XL, 5 = New 2DS XL: NOT an OG result even when
    // run at 268 MHz (AGENTS.md: a New-3DS clocked run is not an OG pass).
    const bool oldFamily = model == 0 || model == 1 || model == 3;
    // Once per process, at exit: a double divide, not a 64-bit integer helper.
    const uint32_t meanUs = g_count ? (uint32_t)((double)g_sumUs / (double)g_count) : 0;
    mdk::log3dsPrintf("profile: frames=%lu mean=%lu.%03lums p50<=%lu.%01lums p95<=%lu.%01lums "
                      "p99<=%lu.%01lums max=%lu.%03lums (frame %lu) over16.7ms=%lu over33.3ms=%lu",
                      (unsigned long)g_count,
                      (unsigned long)(meanUs / 1000), (unsigned long)(meanUs % 1000),
                      (unsigned long)(percentileUs(50) / 1000), (unsigned long)(percentileUs(50) % 1000 / 100),
                      (unsigned long)(percentileUs(95) / 1000), (unsigned long)(percentileUs(95) % 1000 / 100),
                      (unsigned long)(percentileUs(99) / 1000), (unsigned long)(percentileUs(99) % 1000 / 100),
                      (unsigned long)(g_maxUs / 1000), (unsigned long)(g_maxUs % 1000),
                      (unsigned long)g_maxAt, (unsigned long)g_over17, (unsigned long)g_over34);
    const uint32_t steadyUs =
        g_steadyCount ? (uint32_t)((double)g_steadySumUs / (double)g_steadyCount) : 0;
    mdk::log3dsPrintf("profile: steady (frame %lu on) frames=%lu mean=%lu.%03lums; vblanks 1:%lu "
                      "2:%lu 3:%lu 4+:%lu",
                      (unsigned long)kSteadyFrom, (unsigned long)g_steadyCount,
                      (unsigned long)(steadyUs / 1000), (unsigned long)(steadyUs % 1000),
                      (unsigned long)g_vblanks[1], (unsigned long)g_vblanks[2],
                      (unsigned long)g_vblanks[3], (unsigned long)g_vblanks[4]);
    mdk::log3dsPrintf("profile: console model=%u family=%s clock=%s l2=%s stereo=%s "
                      "(frame period = wall clock between frames, first frame excluded; "
                      "percentiles are 0.1 ms bucket upper edges)",
                      (unsigned)model, oldFamily ? "old3ds" : (model == 0xFF ? "unknown" : "new3ds"),
                      oldFamily ? "268MHz(by-model)" : "unverified", oldFamily ? "none(by-model)" : "unverified",
                      g_stereoSeen ? "on" : "off");
#endif
}

const char* harnessCapturePath(uint32_t frame) {
#ifdef MDK_HW_CAPTURE
    if (frame == (uint32_t)(MDK_HW_CAPTURE)) {
        return "sdmc:/3ds/mdk/hwcap.bmp.log";
    }
#else
    (void)frame;
#endif
    return nullptr;
}

void harnessChainloadStub() {
    // Game-side exit recipe (hbmenu pattern): SetTarget + FULL argv, then
    // the caller's exit(0) is the transfer. svcConnectToPort("hb:ldr")
    // fails without Luma's loader; then this is simply a normal exit.
    static char argvBuffer[0x400];
    const char* path = "/3ds/netload_stub.3dsx";  // sdmc-relative, no prefix
    Handle hbldr = 0;
    if (R_FAILED(svcConnectToPort(&hbldr, "hb:ldr"))) {
        return;
    }
    u32* cmdbuf = getThreadCommandBuffer();
    const u32 pathLength = (u32)strlen(path) + 1;
    cmdbuf[0] = IPC_MakeHeader(2, 0, 2);
    cmdbuf[1] = IPC_Desc_StaticBuffer(pathLength, 0);
    cmdbuf[2] = (u32)path;
    Result rc = svcSendSyncRequest(hbldr);
    if (R_SUCCEEDED(rc)) {
        rc = (Result)cmdbuf[1];
    }
    if (R_FAILED(rc)) {
        svcCloseHandle(hbldr);
        return;  // target not registered: stay a normal exit
    }
    ((u32*)argvBuffer)[0] = 1;
    snprintf(argvBuffer + 4, sizeof(argvBuffer) - 4, "sdmc:%s", path);
    cmdbuf[0] = IPC_MakeHeader(3, 0, 2);
    cmdbuf[1] = IPC_Desc_StaticBuffer(sizeof(argvBuffer), 1);
    cmdbuf[2] = (u32)argvBuffer;
    rc = svcSendSyncRequest(hbldr);
    svcCloseHandle(hbldr);
}

} // namespace mdk

#else  // MDK_AUTOQUIT

namespace mdk {

bool harnessAutoquitDue() {
    return false;
}

const char* harnessCapturePath(uint32_t) {
    return nullptr;
}

uint64_t harnessProfileTick() {
    return 0;
}

void harnessProfileStepSpans(uint64_t, uint64_t, uint64_t, uint64_t) {}

void harnessProfileSpans(uint64_t, uint32_t, uint64_t, uint64_t, uint64_t, uint64_t) {}

void harnessProfileFrame(double) {}

void harnessProfileReport() {}

void harnessChainloadStub() {}

} // namespace mdk

#endif  // MDK_AUTOQUIT
