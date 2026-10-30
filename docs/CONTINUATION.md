# Phase 2 continuation checkpoint

Last updated: 2026-10-30 (review-30: the oracle's complete `Fall` operation and
its death-state semantics). The pinned frame is unchanged. Opcode coverage is
unchanged: 211 true C++ dispatches / 238, 27 deferred.

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
5. **NEXT — review-30's 3DS MAJORs, one renderer slice each:** honour
   `Pass::DoubleSided` in the Citro3D backend, then replace eager Kurt-sprite
   residency with bounded/on-demand textures that the 3DS eviction can restore.
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
`M * v`. Citro3D uses `GPU_CULL_BACK_CCW` with the mirror removed: **UNVERIFIED on
hardware** (P6).

**Sky.** `CameraMath.View` builds the sky's clip-to-direction as
`Invert(CreateLookAt(Zero, camera.Forward, camera.Up) * projection)` — the follow
camera's OWN axes, because the view recovers forward through a cancellation that
changes the ULP. `Matrix4x4.Invert` follows .NET's SSE shuffle/FMA sequence and
`Vector4.Dot`'s pairwise reduction, not a cofactor formula
(`system_numerics_inverse.cpp`); pinned by `PASS SystemNumericsMatrixInverse`.

**Depth layers and the depth pull.** `computeArenaDepthLayers` is the shared half
of the C# `Layers.Of` (rounded plane keys, biggest-first stacking, a smaller
triangle one layer above the highest bigger one it overlaps); the layer joins the
batch key and `MeshDraw::material.depth_layer`. `render/depth_pull.cpp` is the C#
`DepthPull.Of`; Vulkan applies `world * depth_pull * view * projection`.
`layer_tests.cpp` pins the C# cases; forcing every draw nearer changes 0.009%
(max 58), proving the path is live. `renderer_c3d.cpp` does not apply the pull.

**Frame golden.** `tests/frames/reference.sha256` pins level 3, 320x240, seed 1,
frame 1: `oracle=b60e72552c40…` (byte-identical across captures) and
`cpp=58615fc85d02…` (regression baseline, NOT a fidelity reference), gap
`bad_percent=1.240 max_delta=247 mean_delta=0.1819` at tolerance 0. The gate checks
determinism, byte-exact match to `cpp=`, and that the gap does not exceed the
ceiling, which may only shrink. (The `0.3895` mean in the iteration-30 narrative was
a prose transcription error; the manifest and sidecar say `0.1819`.)

**Kurt.** `game/kurt_sprite.{h,cpp}` is the C#'s `Kurt/KurtSprite.cs`: a frame is
a world quad at the feet, one sprite pixel = 1/360 of the view height, hotspot 101
px above the feet, drawn as two row bands — the body pulled in front of the floors
the eye's rays to its 3x3 sample points cross (`ArenaSpace::floorCrossing`), the
rows around the feet pulled two world units nearer still. `Pick` selects the
animation and frame (loop/once wrap, the chute's K_CHUTE 0-4 then K_CHUTEC's sway,
K_CHUTE frame 7 once on `chuteClosing`). `Player` carries the C#'s visual state
machine (`UpdateState`/`Animate`/`UpdateSpecial`/`UpdateDeath`/`SetState`/`RunRate`);
a hard landing enters K_CRASHL and owns the step. States another subsystem owns
(firing, knock-down, slide, ledges, ride, beta) have no writer and their oracle
branches are taken as false.

**Death and `Fall`.** `Player::fall` is the C#'s `Kurt.Fall` whole: `FallMove`
first, then the 50-unit fall-out test; `UpdateVertical`, `UpdateSpecial` and
`UpdateDeath` all call it. On a real floor the zero-length death move clears
`OnFloor` and the next step re-lands with the creep snap; below the arena the
C#'s **post-move** fall-out assignment sets `OnFloor` true again in the same step —
a scratch oracle run held the corpse at Z 52.982224 for 120 steps with
`onFloor=True vertical=0`. Review-30's claim that the oracle corpse keeps falling
stopped before that assignment (`Kurt.cs:491-503`). Both halves are pinned by
`DeathUsesTheOraclesCompleteFallOperation`.

**Animation wrap.** `Player::wrapAnimationFrame` is the exact C# reduction of
`((frame % count) + count) % count` — one remainder and a conditional add, with no
invalid hot-only R11 pin on the cold sprite path.
`WrapAnimationFrameIsTheOracleReduction` pins positive, negative and zero-count.

**Samplers.** The C# has TWO (`Renderer.cs:249-250`), bound per resource in
`DrawOne`: index texture REPEAT, palette CLAMP, sky panorama both CLAMP. Arena UVs
are texels normalised by texture size, so a surface showing a texture twice has a
coordinate above 1 and the original TILES it. Vulkan has `sampler` (clamp) and
`samplerRepeat`; `getDescriptorSet` is keyed by (texture, palette, repeat).

**HUD.** The canvas is 360 units high and `360 * target_width / target_height`
wide (C# `Renderer.CanvasWidth`). No invented inventory slots or target bar, the
blink advances on the C#'s 30 Hz boundary, and a dead player's skull uses the
oracle's 60/s fade to 255. It still respawns in place: the menu/save/level-end
flow is not ported.

**Frontend.** Linux loads CMI/sound bank, runs `ScriptRuntime` after Kurt and
before camera, and rebuilds it on respawn. The follow camera consumes
`cameraTrackPitch` while its request lives, otherwise current arena pitch.
Coverage: C# 238, C++ 213 labels / 212 unique / 211 true dispatches, 27
remaining; 16 partial, 195 complete.

## Verified on this tree

- `make -C tests/scripts clean && make -C tests/scripts` exited 0, ended `All
  cases match the oracle.`; the two new cases passed and no movement golden moved.
- `cmake --build build-linux --target mdk -j16` exited 0.
- `bash tools/check.sh` exited 0 (`CHECK: ALL GATES PASSED`; SMDH SKIPPED for no
  CIA; `gates_frames PASS` with `L3_f1.bmp sha=58615fc85d02e009`).
- `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds`
  exited 0 and built `mdk.3dsx` (69 RomFS files, 79M staged).
- A scratch .NET 10 oracle program on the read-only C# source and real level data:
  after falling out the corpse held Z 52.982224 for 120 steps, `OnFloor=True`,
  `VerticalSpeed=0`.
- `tools/publish_frame.sh review30-fall` wrote
  `docs/frames/2026-10-30-review30-fall.png` + `.txt` with
  `bad_percent=1.240 max_delta=247 mean_delta=0.1819` — unchanged, as expected
  with no renderer change.
- No emulator or Old-3DS hardware run was performed. Runtime combat/inventory
  `Kurt` stays separate from movement `Player`; object rendering, sub-step frame
  blending and the 3DS frontend binding stay deferred.

**Next increment:** review-30's 3DS `Pass::DoubleSided` finding. Set culling
explicitly per draw in `renderer_c3d.cpp` (`GPU_CULL_NONE` for DoubleSided,
`GPU_CULL_BACK_CCW` for Solid), then build and inspect the 3DS object. Do not
emit duplicate windings. The texture-residency MAJOR follows as its own batch.

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
- **C3D depth state still differs from the C#** (test disabled vs LESS_OR_EQUAL
  test/write), which is also why `renderer_c3d.cpp` ignores `depth_layer`.
- Review-30's 3DS culling and texture-residency MAJORs, and the narrower
  `KurtSprite` replacement/failure texture leaks, are recorded with measurements.
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
