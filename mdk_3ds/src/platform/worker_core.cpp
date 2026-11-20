// ============================================================================
// worker_core.cpp -- see worker_core.h.
// ============================================================================

#include "platform/worker_core.h"

#include <3ds.h>

#include "platform/log_3ds.h"
#include "platform/vfp_fpscr.h"

namespace mdk {

namespace {

#ifdef MDK_AUTOQUIT
// THE CORE PROBE'S THREAD (harness builds only): it does nothing but prove it
// ran. A plain function so the R1 gate (tools/runner/gates_vfp.py) resolves the
// routine threadCreate names and reads its body.
void coreProbeThread(void* ran) {
    // R1, AT THE WORKER'S ENTRY, as every MDK thread carries it: FZ and DN are
    // MDK's chosen numerical mode and FPSCR is per thread, so the main thread's
    // state is not evidence of this one's. This thread does no float work, but
    // decaying values can reach subnormals in any float code a later change
    // adds; modern libctru already initialises the mode on its supported entry,
    // so this is the defensive read-back the contract asks for.
    const bool fzdn = vfpThreadFzDn();
    *static_cast<bool*>(ran) = fzdn;
}

// Does `core` accept an application thread? Created, joined (one second: the
// thread returns at once), freed. A thread that is not back in time is logged
// and never freed -- freeing a running thread's object is worse than the leak.
const char* probeCore(int core) {
    bool ran = false;
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    const s32 probePriority = priority < 0x3F ? priority + 1 : priority;
    Thread thread = threadCreate(coreProbeThread, &ran, 4096, probePriority, core, false);
    if (thread == nullptr) {
        return "refused";
    }
    if (R_FAILED(threadJoin(thread, 1000ULL * 1000ULL * 1000ULL))) {
        return "created, join TIMED OUT (not freed)";
    }
    threadFree(thread);
    return ran ? "ok" : "ran, FZ/DN read-back FAILED";
}
#endif

}  // namespace

void workerCoreInit() {
    bool isNew = false;
    APT_CheckNew3DS(&isNew);
#ifdef MDK_HW_WORKERCORE_DEFAULT
    log3dsPrintf("cpu: %s; WORKERCORE=0 A/B: workers on core %d, no syscore time limit set",
                 isNew ? "New 3DS" : "Old 3DS", kWorkerCore);
#else
    // Idempotent, and harmless on the New 3DS (t2k calls it on both). A failure
    // is not fatal: each worker's threadCreate falls back to the default core
    // and logs which core it got.
    const Result set = APT_SetAppCpuTimeLimit(kSyscoreTimeLimitPercent);
    u32 granted = 0;
    const Result got = APT_GetAppCpuTimeLimit(&granted);
    log3dsPrintf("cpu: %s; syscore time limit %lu%% -> 0x%08lX (read back %lu%%, 0x%08lX); "
                 "workers on core %d, fallback %d",
                 isNew ? "New 3DS" : "Old 3DS", (unsigned long)kSyscoreTimeLimitPercent,
                 (unsigned long)set, (unsigned long)granted, (unsigned long)got, kWorkerCore,
                 kWorkerCoreFallback);
#endif
#ifdef MDK_AUTOQUIT
    // THE CORE PROBE (harness builds only, never a release): settles whether an
    // app may put a thread on core 2 and on core 3 of a New 3DS. On the Old 3DS
    // both are expected to be refused (the cores do not exist).
    const char* core2 = probeCore(2);
    const char* core3 = probeCore(3);
    log3dsPrintf("cpu: probe core 2 %s, core 3 %s", core2, core3);
#endif
}

}  // namespace mdk
