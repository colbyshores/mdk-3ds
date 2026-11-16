#pragma once

// ============================================================================
// harness_3ds.h -- the autonomous hardware-loop hooks (netload_stub cycle).
//
// The unattended cycle on a real 3DS is: 3dslink pushes the .3dsx, the
// game runs N seconds, quits BY ITSELF, and chain-loads the stub so the
// next push has somewhere to land (Obsidian runbook
// 20-runbooks/3ds-autonomous-hardware-profiling-loop.md; ledger row
// 3ds.harness-hooks, the host-verifiable half of the hw.* rows).
//
// Two hooks, both compiled in ONLY under -DMDK_AUTOQUIT=<seconds>
// (mdk_3ds/Makefile's AUTOQUIT flag), so a release build carries neither:
//
//   * harnessAutoquitDue() -- armed on the first frame (never at init),
//     true once the deadline expires. Uses svcGetSystemTick() +
//     SYSCLOCK_ARM11, the svc whose name nothing else in the tree can
//     collide with (the runbook's osGetTime lesson).
//
//   * harnessChainloadStub() -- registered with atexit in a harness
//     build: hb:ldr SetTarget + full SetArgv for the stub, best-effort.
//     NEVER DoApplicationJump (proven to wedge APT on Old 3DS hardware);
//     the exit that follows is the transfer.
// ============================================================================

namespace mdk {

// True once the harness deadline expires (first call arms it). Always
// false in a build without MDK_AUTOQUIT.
bool harnessAutoquitDue();

// Chain-load the netload stub on exit (harness builds only; a no-op in
// any other build, and best-effort even there: no Luma, normal exit).
void harnessChainloadStub();

} // namespace mdk
