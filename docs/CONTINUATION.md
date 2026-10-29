# Phase 2 continuation checkpoint

Last updated: 2026-10-29 (P5 sampler address mode). Opcode coverage is
unchanged: 211 true C++ dispatches / 238, 27 deferred. The frame gap fell from
7.930% to **4.398%**; P5 continues one renderer slice at a time.

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
5. **P5 — close the measured 4.398% frame gap**, one renderer slice at a time.
   The largest remaining band is the sky (top third: 5.625% of its pixels
   differ, mean 1.49, max 99 — a washed-out panorama). The bottom band carries
   the largest mean delta (4.38, ground and HUD). The 3DS's half of this batch's
   sampler fix is deferred with cause below.

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
frame 1: `oracle=b60e72552c40…` (unchanged since first captured),
`cpp=f4f91150cd35…` (regression baseline), gap `bad_percent=4.398 max_delta=255
mean_delta=1.4939` at tolerance 0. The gate checks determinism, byte-exact match
to `cpp=`, and that the oracle gap does not exceed the ceiling. The ceiling may
only shrink; the gap is open P5 work, not a fidelity pass.

**Samplers.** The C# has TWO (`Renderer.cs:249-250`), bound per resource in
`DrawOne` (`:1098-1099`): index texture REPEAT, palette CLAMP, sky panorama both
CLAMP (`:1029-1030`). The arena's UVs are texels normalised by texture size, so
a surface showing a texture twice has a coordinate above 1 and the original
TILES it; clamping stretched the edge texel and turned walls flat brown. Vulkan
now has `sampler` (clamp) and `samplerRepeat`, and `getDescriptorSet` is keyed by
(texture, palette, repeat): mesh and canvas ask for repeat, the sky for clamp.

**HUD.** The seam exposes `mdk_render_canvas_width`: the canvas is 360 units high
and `360 * target_width / target_height` wide, matching C# `Renderer.CanvasWidth`;
Vulkan's canvas ortho and the HUD layout use it. The frontend invents no
inventory slots and no target bar (a fresh C# level has an empty inventory and
`GetBar() == (0, 0)`), the blink advances on the C#'s 30 Hz boundary, and a dead
player's skull uses the C#'s 60/s fade to 255. It still respawns in place because
menu/save/level-end flow is not ported.

**Frontend.** Linux loads CMI/sound bank, runs `ScriptRuntime` after Kurt and
before camera, and rebuilds it on respawn. Follow camera consumes `cameraTrackPitch`
while its request lives, otherwise current arena pitch. Capture CLI:
`--headless --fixed-step --capture= --capture-frame=N --size=WxH --level=N --at=x,y,z[,yaw]`.

**Coverage unchanged:** C# 238, C++ 213 labels / 212 unique / 211 true dispatches,
27 remaining; 16 partial, 195 complete.

**Verification on this tree:** `make -C tests/scripts clean && make -C
tests/scripts` exited 0 ending `All cases match the oracle.`; `bash tools/check.sh`
exited 0 (`CHECK: ALL GATES PASSED`, SMDH SKIPPED for no CIA, `gates_frames PASS`
with `L3_f1.bmp sha=f4f91150cd35fd56`); `DEVKITPRO=/opt/devkitpro
DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds` exited 0, linked `mdk.elf`,
staged 69 RomFS files (79M), built `mdk.3dsx`; `cmake --build build-linux --target
mdk -j8` exited 0; `tests/frames/regenerate_reference.sh` exited 0 from a real
oracle run on display `:99` in a private namespace (lavapipe/llvmpipe,
`Level 3: 17 arenas, 9708 triangles`, `Kurt at -4.00 0.00 192.98 yaw 96`).

**No VM golden changed. No emulator or Old-3DS hardware run was performed.**
Old-3DS timing is unmeasured and the Citro3D cull enum is unverified on hardware.
Runtime combat/inventory `Kurt` remains separate from movement `Player`; object
rendering and 3DS frontend binding stay deferred. C++ uses one seeded legacy RNG
per process, unlike C#'s fresh unseeded generator. Review-21 caveat stands:
post-camera solid refresh pins only the current arena, not C#'s `SolidArenas`.

**Next increment: P5 sky.** The top third is the largest band and its max delta
is only 99, so it is a washed-out panorama, not missing geometry: compare the
C++ sky's clip-to-direction and panorama sampling against the oracle there, then
take the bottom band (mean 4.38: ground shading and HUD).

**The 3DS half of the sampler fix is deferred with cause.** `renderer_c3d.cpp`
never calls `C3D_TexSetWrap`, so its wrap field is 0 — not `GPU_REPEAT`
(`enums.h:39`) — and it clamps like Vulkan used to. It cannot take the same
one-liner: the 3DS pads index textures to POT with TRANSPARENT padding and
rescales UVs by real/padded in the vertex shader (`renderer_c3d.cpp:1133-1147`),
so a plain REPEAT tiles at the PADDED period and samples the padding — index 0,
which discards — punching holes instead of tiling. It needs the padding filled
with the tile pattern, and it is unverifiable without hardware.

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
- Review-24's MINOR (the death-beat comment said 4.0 s after the constant became
  4.25 s) and both NITs are fixed. Its open question stands: VK `canvasWidth`
  reads the swapchain extent while the C# reads the render target —
  indistinguishable while both are 4:3.
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
