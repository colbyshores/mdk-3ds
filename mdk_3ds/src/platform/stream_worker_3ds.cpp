// ============================================================================
// stream_worker_3ds.cpp -- see stream_worker_3ds.h.
// ============================================================================

#include "platform/stream_worker_3ds.h"

#include "audio/audio_dsp_3ds.h"
#include "platform/async_file_reader.h"
#include "platform/log_3ds.h"
#include "platform/vfp_fpscr.h"

namespace mdk {

namespace {

constexpr size_t kStackBytes = 16 * 1024;  // the music worker's and the reader's
constexpr uint64_t kTicksPerMs = SYSCLOCK_ARM11 / 1000ull;
constexpr s64 kMusicTickNs = 25 * 1000 * 1000;  // the music's refill tick
constexpr uint64_t kMusicTickTicks = 25 * kTicksPerMs;
// The log's lines wait at most this long behind reads (or until its queue is
// half full), so a burst of reads never fills it -- a full queue is written by
// its caller, which may be the main thread.
constexpr uint64_t kLogMaxWaitTicks = 100 * kTicksPerMs;
// The worker's own summary (`stream:`), every this long.
constexpr uint64_t kSayEveryTicks = 10000 * kTicksPerMs;

Thread g_thread = nullptr;
bool g_running = false;       // main thread: set before the thread starts, cleared after the join
int32_t g_madeOn = -2;
LightLock g_stateLock;
bool g_exit = false;          // guarded by g_stateLock
int32_t g_attachWaiting = 0;  // guarded by g_stateLock: main is waiting to attach or detach
LightEvent g_wake;
// The services: guarded by g_serviceLock, which the worker holds for its whole
// pass over them (stream_worker_3ds.h, LIFETIME AND OWNERSHIP).
LightLock g_serviceLock;
bool g_audio = false;
AsyncFileReader* g_reader = nullptr;

// THE WORKER'S ENTRY, a plain function so the R1 gate (tools/runner/gates_vfp.py)
// resolves the routine threadCreate names and reads its body.
void streamWorkerThread(void*) {
    // R1, AT THE WORKER'S ENTRY. Flush-to-zero and default-NaN are MDK's chosen
    // numerical mode, and FPSCR is per thread: the main thread's state is not
    // evidence of this one's. This worker does float work -- the music channels'
    // (re)configuration sets their rate, mix and filter coefficients -- and
    // decaying values can reach subnormals in float code; modern libctru already
    // initialises the mode on its supported thread entry, so this is the
    // defensive, read-back form the contract asks every float thread to carry.
    const bool fzdn = vfpThreadFzDn();
    log3dsPrintf("stream: worker running on core %d (FZ/DN %s)", (int)svcGetProcessorID(),
                 fzdn ? "ok" : "read-back FAILED");
    uint64_t lastMusic = 0;
    uint64_t lastLog = svcGetSystemTick();
    uint64_t sayFrom = lastLog;
    uint64_t musicTicks = 0;
    uint64_t readTicks = 0;
    uint64_t logTicks = 0;
    uint32_t passes = 0;
    uint32_t chunks = 0;
    int32_t depthMax = 0;
    for (;;) {
        LightLock_Lock(&g_stateLock);
        const bool exit = g_exit;
        const bool attachWaiting = g_attachWaiting > 0;
        LightLock_Unlock(&g_stateLock);
        if (exit) {
            break;
        }
        if (attachWaiting) {
            // LightLock is not fair: re-taken at once, the service lock could
            // keep an attach or detach (a level's audio close, the renderer's
            // reader stop) waiting through the whole read backlog. 1 ms off it.
            svcSleepThread(1000000LL);
        }
        const uint64_t t0 = svcGetSystemTick();
        bool more = false;
        LightLock_Lock(&g_serviceLock);
        // 1. The music: its tick, or a request it was woken for.
        if (g_audio) {
            const bool tickDue = t0 - lastMusic >= kMusicTickTicks;
            if (tickDue) {
                lastMusic = t0;
            }
            audioDspWorkerPass(tickDue);
        }
        const uint64_t t1 = svcGetSystemTick();
        // 2. One chunk of the best-ranked read.
        if (g_reader != nullptr) {
            const int32_t depth = g_reader->queuedReads();
            depthMax = depth > depthMax ? depth : depthMax;
            more = g_reader->step();
            chunks += depth > 0 ? 1u : 0u;
        }
        LightLock_Unlock(&g_serviceLock);
        const uint64_t t2 = svcGetSystemTick();
        // 3. The log, last: when nothing else waits, or its lines have waited.
        const size_t queued = log3dsQueuedBytes();
        if (queued == 0) {
            lastLog = t2;
        } else if (!more || t2 - lastLog >= kLogMaxWaitTicks || queued >= log3dsQueueBytes() / 2) {
            log3dsDrain();
            lastLog = svcGetSystemTick();
        }
        const uint64_t t3 = svcGetSystemTick();
        musicTicks += t1 - t0;
        readTicks += t2 - t1;
        logTicks += t3 - t2;
        passes++;
        if (t3 - sayFrom >= kSayEveryTicks) {
            const double wall = (double)(t3 - sayFrom);
            log3dsPrintf("stream: %.1f s: busy %.0f%% (music %.1f%%, reads %.1f%%, log %.1f%%), "
                         "%u passes (%u with reads queued), read queue max %d",
                         wall / (double)(kTicksPerMs * 1000ull),
                         100.0 * (double)(musicTicks + readTicks + logTicks) / wall,
                         100.0 * (double)musicTicks / wall, 100.0 * (double)readTicks / wall,
                         100.0 * (double)logTicks / wall, (unsigned)passes, (unsigned)chunks,
                         (int)depthMax);
            sayFrom = t3;
            musicTicks = readTicks = logTicks = 0;
            passes = chunks = 0;
            depthMax = 0;
        }
        if (!more) {
            LightEvent_WaitTimeout(&g_wake, kMusicTickNs);
        }
    }
    log3dsDrain();
}

} // namespace

bool streamWorkerStart() {
    if (g_running) {
        return true;
    }
    LightLock_Init(&g_stateLock);
    LightLock_Init(&g_serviceLock);
    LightEvent_Init(&g_wake, RESET_ONESHOT);
    g_exit = false;
    g_attachWaiting = 0;
    g_audio = false;
    g_reader = nullptr;
    // Core 1's share first: the kernel makes no application thread there without it.
    const bool granted = R_SUCCEEDED(APT_SetAppCpuTimeLimit(kSyscorePercent));
    u32 readBack = 0;
    APT_GetAppCpuTimeLimit(&readBack);
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    // One below the main thread (a larger number is a lower priority). On core 1
    // the main thread is not its rival; on the fallback core a worker that reads
    // without pause must not take the frame's time -- there the music refills
    // only in main's waits (its eight queued seconds cover a busy stretch), one
    // more reason the fallback is said as OFF THE TWO-CORE LAYOUT.
    const s32 workerPriority = priority < 0x3F ? priority + 1 : priority;
    g_madeOn = granted ? 1 : -2;
    g_thread = threadCreate(streamWorkerThread, nullptr, kStackBytes, workerPriority, g_madeOn,
                            false);
    if (g_thread == nullptr && g_madeOn != -2) {
        g_madeOn = -2;
        g_thread = threadCreate(streamWorkerThread, nullptr, kStackBytes, workerPriority, -2,
                                false);
    }
    g_running = g_thread != nullptr;
    log3dsPrintf("stream: core 1 share %u%% %s (reads back %u%%); worker %s on core %d, priority "
                 "0x%X, stack %u%s",
                 (unsigned)kSyscorePercent, granted ? "granted" : "REFUSED", (unsigned)readBack,
                 g_running ? "made" : "FAILED", (int)g_madeOn, (unsigned)workerPriority,
                 (unsigned)kStackBytes,
                 g_running && g_madeOn == 1 ? "" : " -- OFF THE TWO-CORE LAYOUT");
    return g_running;
}

void streamWorkerStop() {
    if (!g_running) {
        return;
    }
    LightLock_Lock(&g_stateLock);
    g_exit = true;
    LightLock_Unlock(&g_stateLock);
    LightEvent_Signal(&g_wake);
    threadJoin(g_thread, U64_MAX);
    threadFree(g_thread);
    g_thread = nullptr;
    g_running = false;
}

bool streamWorkerRunning() { return g_running; }

void streamWorkerWake() {
    if (g_running) {
        LightEvent_Signal(&g_wake);
    }
}

void streamWorkerAttachAudio(bool attached) {
    if (!g_running) {
        return;
    }
    LightLock_Lock(&g_stateLock);
    g_attachWaiting++;
    LightLock_Unlock(&g_stateLock);
    LightLock_Lock(&g_serviceLock);
    g_audio = attached;
    LightLock_Unlock(&g_serviceLock);
    LightLock_Lock(&g_stateLock);
    g_attachWaiting--;
    LightLock_Unlock(&g_stateLock);
    LightEvent_Signal(&g_wake);
}

void streamWorkerAttachReader(AsyncFileReader* reader) {
    if (!g_running) {
        return;
    }
    LightLock_Lock(&g_stateLock);
    g_attachWaiting++;
    LightLock_Unlock(&g_stateLock);
    LightLock_Lock(&g_serviceLock);
    g_reader = reader;
    LightLock_Unlock(&g_serviceLock);
    LightLock_Lock(&g_stateLock);
    g_attachWaiting--;
    LightLock_Unlock(&g_stateLock);
    LightEvent_Signal(&g_wake);
}

} // namespace mdk
