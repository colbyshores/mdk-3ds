# Phase 2 continuation checkpoint

Last updated: 2026-10-29 (P5 HUD canvas/state fidelity). Opcode coverage is
unchanged: 211 true C++ dispatches / 238, 27 deferred. The frame gap fell from
12.680% to **7.930%**; P5 continues one renderer slice at a time.

This is persistent handoff. Keep it under ~150 lines; batch narrative goes in
`docs/PROGRESS_LOG.md`.

## PRIORITY ORDER (owner directive 2026-10-25)

1. **DONE — mixer call-sites** (`ObjectMotion.cs:133`, `ObjectBehaviors.cs:90-115`).
2. **DONE — `Debris.Shatter` + opcodes 137/138/139**, dump line and oracle
   goldens. BreakUp/Trail/Updraft await Effects/Fans.
3. **DONE for the C# oracle — opcode conversion.** 211 dispatches are implemented
   and pinned in `docs/OPCODE_COVERAGE.md`. The Linux frontend constructs
   `ScriptRuntime`, binds the live `Player`, runs its 30 Hz clock after each
   60 Hz player step, and feeds `camera_track`/the current arena to the camera.
   The remaining 27 each need a deferred subsystem. Do not blind-port them or
   reverse-engineer them from `MDKD3D.EXE` (owner directive 2026-10-28).
4. **DONE — P5.0 frame-golden harness.** Oracle capture runs on `:99` in a
   private namespace; C++ capture is fixed-step; `gates_frames.py` is gate 7.
   Proven to fail on a seeded regression.
5. **P5 — close the measured 7.930% frame gap**, one renderer slice at a time.
   The next largest visible slice is the sky panorama; remaining central geometry
   and arena texture differences follow.

Deferred with cause: 248 `push_kurt` awaits knock-back; Rides writes the alarm
countdown; consumers of 135/195/202/203/81 remain partial. Old-3DS timing remains
UNMEASURED.

## Current tree state (VERIFIED)

**Camera and renderer.** `buildLookAtCamera` uses the C#'s RIGHT-HANDED
`CreateLookAt` basis and `CreatePerspectiveFieldOfView` projection. The old
left-handed build mirrored x/z and culled front faces. Vulkan folds clip.y into
the combined matrix (`flipClipY`); the sky inverse carries the flip on its LEFT
side; shaders use `M * v`; canvas ortho is written straight for the y-down
framebuffer. Citro3D uses `GPU_CULL_BACK_CCW` with the mirror removed:
**UNVERIFIED on hardware** (P6).

**Frame golden.** `tests/frames/reference.sha256` pins level 3, 320x240, seed 1,
frame 1. It records `oracle=b60e72552c40…` (the reference), `cpp=4da3474d1c55…`
(the regression baseline), and the measured gap `bad_percent=7.930 max_delta=255
mean_delta=2.1995` at tolerance 0. The gate checks determinism, byte-exact match
to `cpp=`, and that the oracle gap does not exceed the ceiling. The ceiling may
only shrink; the gap is open P5 work, not a fidelity pass.

**HUD.** The seam now exposes `mdk_render_canvas_width`: the canvas is 360 units
high and its width is `360 * target_width / target_height`, matching C#
`Renderer.CanvasWidth`. Vulkan's canvas ortho and HUD layout use it instead of a
fixed 640 width. The Linux frontend no longer invents inventory slots or a target
bar: a fresh C# level has an empty inventory and `GetBar() == (0, 0)`. The HUD
blink advances on the C#'s 30 Hz boundary, and a dead player's skull uses the
C#'s 60/s fade to 255. The frontend still respawns in place because menu/save/
level-end flow is not ported.

**Frontend.** Linux loads CMI/sound bank, runs `ScriptRuntime` after Kurt and
before camera, and rebuilds it on respawn. Follow camera consumes `cameraTrackPitch`
while its request lives, otherwise current arena pitch. Capture CLI is
`--headless --fixed-step --capture= --capture-frame=N --size=WxH --level=N
--at=x,y,z[,yaw]`.

**Coverage unchanged:** C# 238, C++ 213 labels / 212 unique / 211 true dispatches,
27 remaining; 16 partial, 195 complete.

**Verification on this tree:** `make -C tests/scripts clean && make -C
tests/scripts` exited 0 ending `All cases match the oracle.`; `bash tools/check.sh`
exited 0 (`CHECK: ALL GATES PASSED`, SMDH SKIPPED for no CIA, `gates_frames PASS`
with `L3_f1.bmp sha=4da3474d1c5533f0`); `DEVKITPRO=/opt/devkitpro
DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds` exited 0, linked `mdk.elf`,
staged 69 RomFS files (79M), built `mdk.3dsx`; `cmake --build build-linux --target
mdk -j8` exited 0; `tests/frames/regenerate_reference.sh` exited 0 from a real
oracle run on display `:99` in a private namespace (lavapipe/llvmpipe,
`Level 3: 17 arenas, 9708 triangles`, `Kurt at -4.00 0.00 192.98 yaw 96`).

**No VM golden changed. No emulator or Old-3DS hardware run was performed.**
Old-3DS timing is unmeasured and Citro3D cull enum is unverified on hardware.
Runtime combat/inventory `Kurt` remains separate from movement `Player`; object
rendering and 3DS frontend binding stay deferred. C++ uses one seeded legacy RNG
per process, unlike C#'s fresh unseeded generator. Review-21 caveat stands:
post-camera solid refresh pins only current arena, not C#'s full `SolidArenas`.

**Next increment: P5 sky.** Compare the C++ sky's clip-to-direction and panorama
sampling against the oracle's remaining top-third difference, then continue with
arena texture/object slices. All 27 remaining opcodes need deferred subsystems.

## Testability and standing open items

- Oracle/goldens compare fixed table + script fields, not velocity, `alarm_ticks`,
  `Items.Bomb`/`Decoy`, or all combat/effect state. Use `vm_tests` for otherwise
  unreachable opcode behavior. Tick corpus matches through t30000 on levels 3-8.
- Oracle driver: `/tmp/kilo/oracle/Driver/Program.cs`, args `L T 1 script`, with
  `DOTNET_ROOT=/tmp/kilo/dotnet10` and `MDK_DATA_DIR=<repo>/MDK`. Source oracle
  `src/Mdk.Game/**` is read-only; scratch tree is built by `capture_oracle.sh`.
- `--level=N` (3-8) selects level in `LevelData::load`, `LEVEL%d.CMI`, and
  `SoundBank::forLevel`; default remains 3.
- Dynamic scene refresh exists at scene level; no GameEngine exists to wire
  opcode 140. Do not fake call-site wiring.
- Known divergences: incomplete decoy/`AlienTarget` selection; opcode 207's
  `MathF.Sign(NaN)` exception is accepted because C++ exceptions are banned;
  degenerate-path guards return 0. Linux runtime combat/inventory is not yet the
  live `Player`; 3DS frontend has no runtime binding.
- Oracle capture environment: glslangValidator needs `-D` and attached `-I<dir>`;
  `NUGET_PACKAGES` must reach `ppy.SDL3-CS`; host `/tmp/.X11-unix`/`:99` lock is
  unusable, so oracle runs `Xvfb :99` inside `unshare --user --map-root-user -m`
  with private `/tmp/.X11-unix`. Do not simplify either workaround.
- C3D depth state differs from C#: C3D depth test disabled versus C# LESS_OR_EQUAL
  test/write. Resolution needs projection remap, `C3D_DepthMap`, test function,
  clear value and capture. DepthLayer/DepthPull remain unported.
- Old 3DS hardware timing UNMEASURED. Emulator GUI runs must explicitly use
  `DISPLAY=:99`; `/tmp/kilo` is not writable, so captures go under
  `/tmp/mdk-frame-reference` or `/tmp/mdk-pi`, with display recorded.
- Documentation drift: `AGENTS.md`/`SPEC.md` cite absent `docs/DOCTRINE.md`;
  SPEC §9.4 names unwritten `gates_romfs`. Flag to owner; do not invent policy.

## Verification commands

```
make -C tests/scripts clean && make -C tests/scripts
bash tools/check.sh
DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds
python3 tools/runner/gates_frames.py        # gate 7 on its own
tests/frames/regenerate_reference.sh        # only for an intentional frame change
```

Tests do not track header dependencies: run `make -C tests/scripts clean` after
header changes. Keep the monolithic `ScriptVm::execute()` switch. Never weaken
a gate or test.
