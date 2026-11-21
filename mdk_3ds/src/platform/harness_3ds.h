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

// THE ARENA-ENTRY TABLE (PROFILE builds; a no-op otherwise): the owner's "hitch
// when entering a room", measured. Once a frame, after the frame's spans, main
// hands over what the frame did that only happens on first use -- cumulative
// counters, the harness takes the differences -- and the arena pair the scripts
// named. When the pair changes (Kurt's current arena, the second arena or its
// activation), the stream switches, or a view is built, the frames from
// kArenaBefore before it to kArenaAfter after it are kept in RAM with their
// periods (filled in by harnessProfileFrame at the next frame's start) and spans.
// NOTHING is written to the SD card inside the measured frames: the table is
// printed by harnessProfileReport at exit (AGENTS.md lessons: no SD write in a
// play or profile frame). O(1) per frame, fixed storage, no allocation.
struct HarnessArenaFrame {
    const char* arena = "";      // the scripts' current arena (the start arena before)
    const char* second = "";     // the scripts' second arena ("" for none)
    bool secondActive = false;
    int32_t streamSwitches = 0;  // ArenaStream::Stats::switches (cumulative)
    int64_t streamBytes = 0;     // ArenaStream::Stats::bytesRead (cumulative)
    int32_t streamStalls = 0;    // ArenaStream::Stats::stalls (cumulative)
    float streamMs = 0.0f;       // this frame's stream.update
    int32_t viewsBuilt = 0;      // this frame's arenaViews.sync
    float viewsMs = 0.0f;        // ... and its time
    int32_t texturesMade = 0;    // render_c3d_stats.h (cumulative)
    int32_t hdImported = 0;      // render_c3d_stats.h (cumulative)
    int32_t kurtReads = 0;       // KurtSprite::StreamStats::reads (cumulative)
    int32_t kurtHolds = 0;       // KurtSprite::StreamStats holds + holdMisses (cumulative): draws whose wanted frame was still being read
    uint32_t logLines = 0;       // log3dsLinesWritten() (cumulative SD log writes)
};
void harnessArenaFrame(uint32_t frame, const HarnessArenaFrame& sample);

// THE SCRIPTED WALK (`make AUTOQUIT=90 WALK='forward@60-600;turnleft@600-640'`;
// the grammar is game/walk_script.h's). Harness builds with MDK_HW_WALK only.
// harnessWalkInit() parses the script once, at boot, and logs it (one line per
// segment); false means it was refused and the run must stop -- a WALK build
// that does not walk would profile the wrong scene. harnessWalkApply() writes the
// frame's keys into the Input after the platform poll and before the game reads
// it (the script owns the actions and the look; START still quits), and notes
// each segment's start with Kurt's feet and arena: logged at once, or in a
// PROFILE build kept in RAM and printed at exit (no SD write in a measured frame).
// Without MDK_HW_WALK: init is true and apply a no-op.
struct Input;
bool harnessWalkInit();
void harnessWalkApply(uint32_t frame, Input* input, float x, float y, float z, float yaw,
                      const char* arena);

// True once the harness deadline expires (first call arms it). Always
// false in a build without MDK_AUTOQUIT.
bool harnessAutoquitDue();

// Chain-load the netload stub on exit (harness builds only; a no-op in
// any other build, and best-effort even there: no Luma, normal exit).
void harnessChainloadStub();

} // namespace mdk
