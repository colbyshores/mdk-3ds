# Phase 2 continuation checkpoint

Last updated: 2026-10-29 (P5.0 frame-golden harness + the camera handedness fix
it exposed). Current opcode coverage is unchanged: 211 true C++ dispatches /
238, 27 remaining, each needing a deferred subsystem. P5.0 is now BUILT and its
gate is green, so renderer batches (P5) are unblocked.

This is persistent handoff. Keep it under ~150 lines; batch narrative goes in
`docs/PROGRESS_LOG.md`.

## PRIORITY ORDER (owner directive 2026-10-25)

1. **DONE — mixer call-sites** (`ObjectMotion.cs:133`, `ObjectBehaviors.cs:90-115`).
2. **DONE — `Debris.Shatter` + opcodes 137/138/139**, dump line and oracle
   goldens. BreakUp/Trail/Updraft await Effects/Fans.
3. Continue opcode conversion subsystem by subsystem; tests/goldens in the same
   batch. 211 dispatches are implemented and pinned (the per-opcode list is
   `docs/OPCODE_COVERAGE.md`, which is the authority for what is complete,
   partial or deferred and why). The Linux frontend constructs `ScriptRuntime`,
   binds the live `Player`, runs its 30 Hz clock after each 60 Hz player step,
   and feeds `camera_track`/the scripts' current arena to the follow camera.
   All three oracle `Linked` writers (245/179/228) are ported, so partner pairs
   can feed the 213/214 conditions. The remaining 27 each need a deferred
   subsystem, so opcode conversion is EXHAUSTED for what the C# oracle itself
   supports; do not blind-port them and do not reverse-engineer them from the
   EXE (owner directive 2026-10-28).
4. **DONE — P5.0 frame-golden harness** (`SPEC-COMPLETION.md` §3 item 4). The
   reference source is the C# renderer, headlessly under Linux/lavapipe.
   `tests/frames/capture_oracle.sh` builds and runs it on `:99` in a private
   mount namespace; `tests/frames/capture_cpp.sh` renders the same pinned scene
   from the C++ with `--fixed-step`; `compare_frames.py` compares two 32-bit
   BMPs; `regenerate_reference.sh` writes `tests/frames/reference.sha256`
   (hashes and measured deltas only — no ROM-derived pixels are committed);
   `tools/runner/gates_frames.py` is gate 7 of `tools/check.sh`. Proven to fail
   on a seeded regression. **P5 is now unblocked: the renderer batches close the
   measured 12.680% oracle gap, and the gate's ceiling is what says whether a
   batch moved it the right way.**

Deferred with cause (per-opcode detail in `docs/OPCODE_COVERAGE.md`): 248
`push_kurt` is an honest no-op pending Kurt knock-back; the C#'s Rides subsystem
also writes the alarm countdown and stays deferred with Rides; the consumers of
135/195/202/203/81 remain partial with those integrations. Old-3DS timing remains UNMEASURED.

## Current tree state (VERIFIED)

**Camera and renderer.** `buildLookAtCamera` now builds the C#'s RIGHT-HANDED
`CreateLookAt` basis (`zaxis = normalize(eye-target)`,
`xaxis = normalize(cross(up,zaxis))`, `yaxis = cross(zaxis,xaxis)`) and
`CreatePerspectiveFieldOfView`'s projection (`zScale = -far/range`, `w = -z_view`,
NDC depth still [0,1]). The old left-handed build negated x and z against the
oracle, which mirrors the world and culls the solid geometry's front faces. The
Vulkan backend folds the clip.y flip into the combined matrix (`flipClipY`) rather
than using a negative viewport; the canvas ortho is written straight for the
y-down framebuffer; the sky's clip-to-direction carries the flip on its LEFT side
and is uploaded verbatim; `palette.vert`/`sky.vert` multiply `M * v`.
`vk_context::beginFrame`'s viewport is now positive — the renderer overrode it
anyway, and the pinned frame hash is byte-identical before and after, which is how
that was established rather than argued. The Citro3D cull enum moved to
`GPU_CULL_BACK_CCW` with the mirror removed: **UNVERIFIED on hardware** (P6).
**Frame golden.** `tests/frames/reference.sha256` pins level 3, 320x240, seed 1,
frame 1 (the state after exactly one fixed 1/60 s step). Per frame it records
`oracle=` (the C# bytes — the reference), `cpp=` (the C++ regression baseline, NOT
a fidelity reference) and the measured gap `bad_percent=12.680 max_delta=255
mean_delta=5.1350` at tolerance 0. `gates_frames.py` checks (1) two captures are
byte-identical, (2) the capture matches `cpp=`, (3) the gap does not exceed the
ceiling when the oracle pixels are on disk. The ceiling may only SHRINK as P5
closes the gap; raising one is an accepted regression, and the gap is open P5 work,
NOT a fidelity pass. Proven to fail on a seeded regression: dropping `flipClipY`
from `mdk_render_draw_mesh` made the gate report `the frame changed` and `the gap
to the oracle grew: 76.997% vs 12.680%` and `tools/check.sh` exit 1; the seed was
reverted and the gate is green again.

**Frontend.** The Linux frontend loads the level's CMI and sound bank, builds
`ScriptRuntime` with the live `Player`, and runs it after Kurt moves and before
the camera (`Viewer.cs:505-535`); the runtime keeps the C#'s separate 1/30 s
clock, so a script tick completes every second 1/60 s call, and a fresh runtime is
built on respawn. The follow camera consumes `cameraTrackPitch` while its
two-tick request lives, otherwise the scripts' current arena pitch
(`Viewer.cs:746-747`). New CLI for the harness: `--headless --fixed-step
--capture= --capture-frame=N --size=WxH --level=N --at=x,y,z[,yaw]`; the fixed
step makes a capture a simulation state, not a wall clock.

**Review-22's MAJOR, corrected.** The C# play tick calls `_scripts.Update(Step)`
with NO `Dead` guard (`Viewer.cs:509-512`), so scripts advance through the death
beat in the C# too — that was never the divergence. What differs is the END of the
beat: the C# skull fade is `HurtFlash/SkullFade`, `SkullFade = 2*Ticks = 60/s`,
`SkullEnd = 255` (`Kurt.Combat.cs:26-27,238-253`) — at most ~4.25 s, less if
`HurtFlash` starts partway up — and on completion `Died` fires and `Game.cs:448-456`
writes `SaveGames.LastGame` and leaves for the MENU. The C++ has no menu, save or
level end and respawns in place after a fixed 4.0 s. `main.cpp` says that now.

**Coverage is unchanged by this batch:** C# 238, C++ 213 labels / 212 unique /
211 true dispatches, 27 remaining; 16 partial, 195 complete. Details per opcode
are in `docs/OPCODE_COVERAGE.md`.

**Verification on this tree:** `make -C tests/scripts clean && make -C
tests/scripts` exited 0 ending `All cases match the oracle.`; `bash
tools/check.sh` exited 0 (`CHECK: ALL GATES PASSED`, SMDH SKIPPED for no CIA,
`gates_frames` PASS with `L3_f1.bmp sha=2b586df212dc8ed8`); `DEVKITPRO=/opt/devkitpro
DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds` exited 0, linked `mdk.elf`,
staged 69 RomFS files (79M), built `mdk.3dsx`; `cmake --build build-linux --target
mdk -j8` exited 0; `tests/frames/regenerate_reference.sh` exited 0 and wrote the
manifest from a real oracle run (display `:99` in a private namespace,
lavapipe/llvmpipe, `Level 3: 17 arenas, 9708 triangles`, `Kurt at -4.00 0.00
192.98 yaw 96`).

**No golden changed. No emulator or Old-3DS hardware run was performed**; Old-3DS
timing is unmeasured and the Citro3D cull-enum change is unverified on hardware.
The runtime's combat/inventory `Kurt` is still separate from the movement
`Player`, so health/items/object rendering and the 3DS frontend stay deferred.
C++ runs reuse one seeded legacy RNG per process, unlike the C#'s fresh unseeded
generator per level load. Review-21's caveat stands: the post-camera solid-arena
refresh pins only the scripts' current arena, not the C#'s
current-plus-active-second `SolidArenas` list.

**Next increment: P5** — close the measured 12.680% frame gap toward the oracle,
one renderer slice at a time, with `gates_frames` as the proof. All 27 remaining
opcodes need a deferred subsystem (Effects, Fans, Rides, SniperRounds, teleport,
HUD, wind, waypoints, arena preload, pickups/combat writers); do not blind-port.

## Testability and standing open items

- Oracle/goldens compare fixed table + script fields, not velocity,
  `alarm_ticks`, `Items.Bomb`/`Decoy`, or all combat/effect state. Use
  `vm_tests` for otherwise unreachable opcode behavior; don't port without a
  test. The tick corpus matches byte-for-byte through t30000 on levels 3-8.
- Oracle driver: `/tmp/kilo/oracle/Driver/Program.cs`, args `L T 1 script`, with
  `DOTNET_ROOT=/tmp/kilo/dotnet10` and `MDK_DATA_DIR=<repo>/MDK`. Source oracle
  `src/Mdk.Game/**` is read-only; the repo's own C# `obj/` is not writable here,
  so a scratch tree is built (see `tests/frames/capture_oracle.sh`).
- `--level=N` (3-8) now selects the level in all three places that used to
  hardcode level 3: `LevelData::load`, the `LEVEL%d.CMI` path and
  `SoundBank::forLevel`. The default is still 3.
- Dynamic scene refresh exists at scene level; no GameEngine exists to wire
  refresh on opcode 140. Do not fake call-site wiring.
- Known divergences: incomplete decoy/`AlienTarget` target selection; opcode
  207's `MathF.Sign(NaN)` exception is accepted because C++ exceptions are
  banned; degenerate-path guards return 0 rather than C# out-of-range behavior.
  The Linux frontend's runtime `Kurt` combat/inventory state is not yet the live
  `Player` state, and the 3DS frontend has no `Player`/runtime binding yet.
- Oracle capture environment (this machine): glslangValidator needs `-D` and an
  ATTACHED `-I<dir>`; `NUGET_PACKAGES` must reach a cache holding `ppy.SDL3-CS`;
  the host's `/tmp/.X11-unix`/`/tmp/.X99-lock` belong to another user and `:99` is
  stale, so the oracle runs `Xvfb :99` inside `unshare --user --map-root-user -m`
  with a bind-mounted private `/tmp/.X11-unix`. The reasons are in
  `capture_oracle.sh`'s comments; do not "simplify" either workaround.
- C3D depth state still differs from C#: depth test disabled in C3D versus
  LESS_OR_EQUAL depth test/write in C#. Resolution needs projection remap,
  `C3D_DepthMap`, test function and clear value plus capture. DepthLayer/
  DepthPull are unported and depend on the textured renderer path.
- Old 3DS hardware timing UNMEASURED. Never claim OG readiness without actual
  Old 3DS capture. Emulator GUI runs must use explicit `DISPLAY=:99`; `/tmp/kilo`
  is not writable by this user, so captures go under `/tmp/mdk-frame-reference`
  or `/tmp/mdk-pi` and the display used is recorded in the evidence.
- Documentation drift noted earlier: `AGENTS.md`/`SPEC.md` cite absent
  `docs/DOCTRINE.md`; SPEC §9.4 names an unwritten `gates_romfs`. Flag to owner,
  do not invent policy/gates unilaterally.

## Verification commands

```
make -C tests/scripts clean && make -C tests/scripts
bash tools/check.sh
DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds
python3 tools/runner/gates_frames.py        # gate 7 on its own, with per-frame notes
tests/frames/regenerate_reference.sh        # ONLY when the oracle changed
```

Tests do not track header dependencies: run `make -C tests/scripts clean` after
header changes. Keep the monolithic `ScriptVm::execute()` switch. Never weaken
a gate or test.
