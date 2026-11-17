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
uint32_t g_over17 = 0;   // > 16.667 ms: missed a 60 Hz vblank
uint32_t g_over34 = 0;   // > 33.333 ms: missed a 30 Hz vblank
bool g_stereoSeen = false;

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
    if (us > g_maxUs) g_maxUs = us;
    if (us > 16667) g_over17++;
    if (us > 33333) g_over34++;
    if (osGet3DSliderState() > 0.0f) g_stereoSeen = true;
#else
    (void)periodSeconds;
#endif
}

void harnessProfileReport() {
#ifdef MDK_HW_PROFILE
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
                      "p99<=%lu.%01lums max=%lu.%03lums over16.7ms=%lu over33.3ms=%lu",
                      (unsigned long)g_count,
                      (unsigned long)(meanUs / 1000), (unsigned long)(meanUs % 1000),
                      (unsigned long)(percentileUs(50) / 1000), (unsigned long)(percentileUs(50) % 1000 / 100),
                      (unsigned long)(percentileUs(95) / 1000), (unsigned long)(percentileUs(95) % 1000 / 100),
                      (unsigned long)(percentileUs(99) / 1000), (unsigned long)(percentileUs(99) % 1000 / 100),
                      (unsigned long)(g_maxUs / 1000), (unsigned long)(g_maxUs % 1000),
                      (unsigned long)g_over17, (unsigned long)g_over34);
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

void harnessProfileFrame(double) {}

void harnessProfileReport() {}

void harnessChainloadStub() {}

} // namespace mdk

#endif  // MDK_AUTOQUIT
