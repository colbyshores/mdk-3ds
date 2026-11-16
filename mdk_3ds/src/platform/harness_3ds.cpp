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

void harnessChainloadStub() {}

} // namespace mdk

#endif  // MDK_AUTOQUIT
