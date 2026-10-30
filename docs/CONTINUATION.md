# Phase 2 continuation checkpoint

Last updated: 2026-10-30 (the newest external review's two MINORs are closed).
The pinned frame is unchanged. Opcode coverage is unchanged: 211 true C++
dispatches / 238, 27 deferred.

This is persistent handoff. Keep it under ~150 lines; batch narrative goes in
`docs/PROGRESS_LOG.md`.

## PRIORITY ORDER (owner directive 2026-10-25)

1. **DONE — mixer call-sites** (`ObjectMotion.cs:133`, `ObjectBehaviors.cs:90-115`).
2. **DONE — `Debris.Shatter` + opcodes 137/138/139**, dump line and oracle goldens.
   BreakUp/Trail/Updraft await Effects/Fans.
3. **DONE for the C# oracle — opcode conversion.** 211 dispatches are pinned in
   `docs/OPCODE_COVERAGE.md`; the Linux frontend runs `ScriptRuntime` around the
   live `Player` and feeds the camera. The remaining 27 each need a deferred
   subsystem. Do not blind-port them or reverse-engineer them from `MDKD3D.EXE`
   (owner directive 2026-10-28).
4. **DONE — P5.0 frame-golden harness** (`gates_frames.py` is gate 7; proven to
   fail on a seeded regression).
5. **NEXT — review-30's 3DS texture-residency MAJOR.** Replace eager Kurt-sprite
   residency (~49.9 MiB of POT-padded RGBA8 against SPEC's 6 MB VRAM budget)
   with bounded/on-demand residency, and make 3DS eviction restorable rather than
   destructive. Keep this separate from the frontend binding.
6. **P5 — close the measured 1.240% frame gap**, one renderer slice at a time. The
   bands now (max-channel delta, the comparator's definition): **sky 3.023% /
   mean 0.8185 / max 99** — ~774 scattered pixels, no structure; **middle 0.676% /
   mean 0.3446 / max 247**, and every delta above 128 is one 8x5 patch at
   x 174-181, y 127-131, a detail on Kurt's own sprite; **bottom 0.020% /
   mean 0.0054 / max 58**, effectively exact. The sky is the whole remaining
   visual gap and the Kurt patch is the only fully-wrong place left.

Deferred with cause: 248 `push_kurt` awaits knock-back; Rides writes the alarm
countdown; consumers of 135/195/202/203/81 remain partial; the 3DS sampler fix and
Kurt's muzzle flash, ride pose and footsteps. Old-3DS timing remains UNMEASURED.

## Current tree state (VERIFIED)

**Camera and renderer.** `buildLookAtCamera` uses the C#'s RIGHT-HANDED
`CreateLookAt` basis and perspective projection. Vulkan folds clip.y into the
combined matrix; the sky inverse carries the flip on its LEFT side; shaders use
`M * v`. Citro3D uses `GPU_CULL_BACK_CCW` with the mirror removed, and selects
culling per draw: Solid culls back faces; DoubleSided and the other non-solid
passes do not. The installed mode is cached and reset per eye, so repeated
Solid draws no longer dirty and reflash the effect block on every mesh.
**The 3DS result remains UNVERIFIED on hardware** (P6).

**Sky and depth.** The sky inverse uses the follow camera's own axes and .NET's
exact matrix-inverse reduction, pinned by `PASS SystemNumericsMatrixInverse`.
`computeArenaDepthLayers` and `render/depth_pull.cpp` are the C# `Layers.Of` and
`DepthPull.Of`; Vulkan applies the pull and the C3D backend does not.

**Frame golden.** `tests/frames/reference.sha256` pins level 3, 320x240, seed 1,
frame 1: `oracle=b60e72552c40…` (byte-identical across captures) and
`cpp=58615fc85d02…` (regression baseline, NOT a fidelity reference), gap
`bad_percent=1.240 max_delta=247 mean_delta=0.1819` at tolerance 0. The gate checks
determinism, byte-exact match to `cpp=`, and that the gap does not exceed the
ceiling, which may only shrink. (The `0.3895` mean in the iteration-30 narrative was
a prose transcription error; the manifest and sidecar say `0.1819`.)

**Kurt.** `game/kurt_sprite.{h,cpp}` is the C#'s `Kurt/KurtSprite.cs`: a world quad
at the feet, one sprite pixel = 1/360 of the view height, hotspot 101 px above the
feet, drawn as two row bands with the oracle's floor crossings. `Pick` selects
the animation and frame; `Player` carries the C#'s visual state machine. States
another subsystem owns have no writer and their oracle branches are taken as false.

**Death and `Fall`.** `Player::fall` is the C#'s `Kurt.Fall` whole: `FallMove`
first, then the 50-unit fall-out test; `UpdateVertical`, `UpdateSpecial` and
`UpdateDeath` all call it. Below the arena the C#'s **post-move** fall-out
assignment sets `OnFloor` true again in the same step, so the corpse stays at the
death point. The old build already held the same position; the real divergences
were `OnFloor` **and** `VerticalSpeed` parity. Review-30's mid-air-hang MAJOR
was therefore void, not fixed. The complete operation and its parity are pinned by
`DeathUsesTheOraclesCompleteFallOperation`.

**Animation wrap.** `Player::wrapAnimationFrame` is the exact C# reduction of
`((frame % count) + count) % count` — one remainder and a conditional add, with no
invalid hot-only R11 pin on the cold sprite path.
`WrapAnimationFrameIsTheOracleReduction` pins positive, negative and zero-count.

**Samplers.** The C# has TWO (`Renderer.cs:249-250`): index texture REPEAT, palette
CLAMP, sky panorama both CLAMP. Arena UVs above 1 therefore tile, as in the
oracle; Vulkan keys its descriptor set by (texture, palette, repeat).

**HUD and frontend.** The canvas is 360 units high and the C#'s aspect-derived
width. The blink and skull fade follow the oracle; menu/save/level-end flow is
not ported. Linux loads CMI/sound bank, runs `ScriptRuntime` after Kurt and
before camera, and rebuilds it on respawn. Coverage: C# 238, C++ 213 labels / 212
unique / 211 true dispatches, 27 remaining; 16 partial, 195 complete.

## Verified on this tree

- `make -C tests/scripts clean && make -C tests/scripts -j4` exited 0, ended `All
  cases match the oracle.`; the focused VM cases passed and no golden moved.
  `cmake --build build-linux --target mdk -j16` exited 0.
- `bash tools/check.sh` exited 0 (`CHECK: ALL GATES PASSED`; SMDH SKIPPED for no
  CIA; `gates_frames PASS` with `L3_f1.bmp sha=58615fc85d02e009`).
- `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds -j4`
  exited 0 and built `mdk.3dsx` (69 RomFS files, 79M staged).
- `tools/publish_frame.sh c3d-cull-cache` wrote the new frame pair with
  `bad_percent=1.240 max_delta=247 mean_delta=0.1819` — byte-identical to the
  previous C++ frame, as expected because the change is 3DS-only.
- No emulator or Old-3DS hardware run was performed. The 3DS frontend binding,
  runtime combat/inventory and sub-step frame blending stay deferred.

**Next increment:** review-30's texture-residency MAJOR. Design bounded,
on-demand sprite textures whose source can rebuild an evicted 3DS texture; keep
the oracle's frame selection and vertex count unchanged, and measure the resident
budget before wiring the 3DS frontend.

**Do not "fix" `follow_camera.cpp`'s `normalize`.** It multiplies by a reciprocal
where `Vector3.Normalize` reads as a division. Making it a division REGRESSES the
sky from 3.023% to 5.730% — the reciprocal form is what the oracle matches.

**The 3DS sampler fix stays deferred with cause** — the 3DS pads index textures to
POT with transparent padding, so a plain `C3D_TexSetWrap(REPEAT)` would tile at the
padded period and sample discarded padding (`OPEN_ITEMS.md`).

## Standing open items

Full list with evidence in `docs/OPEN_ITEMS.md`. The ones that bite:

- **Frame record:** every batch publishes `tools/publish_frame.sh <label>` after
  the gates and commits `docs/frames/<date>-<label>.png` + `.txt` WITH the batch.
  The oracle's pixels stay out.
- **C3D depth and blend state still differ from the C#** (test disabled vs
  LESS_OR_EQUAL test/write, and `Pass::Blended` glass is opaque), which is also
  why `renderer_c3d.cpp` ignores `depth_layer`; `OnTop`/`Overlay` depth
  exemptions are not modelled.
- Review-30's texture-residency MAJOR and the narrower `KurtSprite`
  replacement/failure texture leaks remain recorded with measurements.
- The oracle capture environment is not simplifiable (glslang flags,
  `NUGET_PACKAGES`, the `unshare` namespace for `Xvfb :99`).

## Verification commands

```
make -C tests/scripts clean && make -C tests/scripts   # goldens
bash tools/check.sh                                    # all gates
DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds
python3 tools/runner/gates_frames.py                   # gate 7 alone
tests/frames/regenerate_reference.sh                   # intentional frame change only
```

Tests do not track header dependencies: run `make -C tests/scripts clean` after
header changes. Keep the monolithic `ScriptVm::execute()` switch. Never weaken a
gate or test.
