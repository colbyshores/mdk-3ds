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

#include <stdint.h>

namespace mdk {

// The hardware loop's own picture (ledger row hw.pixels-rgba8). In a harness
// build that also carries -DMDK_HW_CAPTURE=<frame> (`make AUTOQUIT=90
// CAPTURE=600`), returns the capture path at exactly that frame and nullptr at
// every other; nullptr always in any other build. The path ends in `.log`
// because the console's ATM HTTP server serves only `/3ds/**.log|*.txt`
// (atm_core.c's suffix allowlist), so a `.bmp` written there could not be
// pulled. The body is an ordinary BMP from the renderer's left-eye readback.
// One capture, never armed in a profiling build: it stalls one frame on SD.
const char* harnessCapturePath(uint32_t frame);

// The hardware loop's first OG profile (ledger row hw.profile-og). In a harness
// build that also carries -DMDK_HW_PROFILE (`make AUTOQUIT=90 PROFILE=1`),
// harnessProfileFrame() records one frame's wall-clock period (the time since
// the previous frame, measured BEFORE the gameplay clamp) into a fixed
// histogram -- O(1), no allocation, no I/O -- and harnessProfileReport() logs
// one `profile:` summary with the console model, the stereo state and the
// percentiles. No-ops in every other build. This is a frame-period capture,
// not a function-level profile: it cannot populate the hot-path corpus.
void harnessProfileFrame(double periodSeconds);
// Where the frame went (PROFILE builds; no-ops otherwise). Raw svcGetSystemTick
// spans: the fixed-step simulation loop and how many steps it ran, the
// render begin (C3D_FrameBegin's vblank wait lives here -- an elastic idle,
// kept in its own bucket), the draw issue between begin and end, and the end.
// harnessProfileTick() is the tick source so main needs no <3ds.h> timing code.
uint64_t harnessProfileTick();
// Inside the simulation step (PROFILE builds): Kurt (player.update), the
// scripts (runtime->update), the follow camera, and space.setSolid, summed.
void harnessProfileStepSpans(uint64_t playerTicks, uint64_t runtimeTicks,
                             uint64_t cameraTicks, uint64_t solidTicks);
// `audioTicks` is the frame's audio work: listenFromCamera, the mixer's update,
// the DSP path's channel sync (audio_dsp_3ds.h) and the device's update -- the
// whole CPU cost of sound in either mode, so a DSPAUDIO=0 A/B reads it directly.
void harnessProfileSpans(uint64_t simTicks, uint32_t simSteps, uint64_t beginTicks,
                         uint64_t drawTicks, uint64_t endTicks, uint64_t audioTicks);
void harnessProfileReport();

// True once the harness deadline expires (first call arms it). Always
// false in a build without MDK_AUTOQUIT.
bool harnessAutoquitDue();

// Chain-load the netload stub on exit (harness builds only; a no-op in
// any other build, and best-effort even there: no Luma, normal exit).
void harnessChainloadStub();

} // namespace mdk
