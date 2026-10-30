# Phase 2 continuation checkpoint

Last updated: 2026-10-30 (Kurt's sprite and his state machine). The frame gap
falls **3.323% -> 1.240%** (max 255 -> 247, mean 2.4058 -> 0.3895): drawing Kurt
clears the middle and bottom bands almost entirely. Opcode coverage is unchanged:
211 true C++ dispatches / 238, 27 deferred.

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
4. **DONE — P5.0 frame-golden harness.** Oracle capture runs on `:99` in a private
   namespace; C++ capture is fixed-step; `gates_frames.py` is gate 7. Proven to fail
   on a seeded regression.
5. **P5 — close the measured 1.240% frame gap**, one renderer slice at a time. The
   bands now (max-channel delta, the comparator's definition): **sky 3.023% /
   mean 0.8185 / max 99** — ~774 scattered pixels, no structure; **middle 0.676% /
   mean 0.3446 / max 247**, and every delta above 128 is one 8x5 patch at
   x 174-181, y 127-131, i.e. a detail on Kurt's own sprite; **bottom 0.020% /
   mean 0.0054 / max 58**, effectively exact. So the sky is the whole remaining
   gap and the Kurt patch is the only fully-wrong place left in the frame.

Deferred with cause: 248 `push_kurt` awaits knock-back; Rides writes the alarm
countdown; consumers of 135/195/202/203/81 remain partial; the 3DS half of the
sampler fix (below) and Kurt's muzzle flash, ride pose and footsteps. Old-3DS
timing remains UNMEASURED.

## Current tree state (VERIFIED)

**Camera and renderer.** `buildLookAtCamera` uses the C#'s RIGHT-HANDED
`CreateLookAt` basis and perspective projection. Vulkan folds clip.y into the
combined matrix; the sky inverse carries the flip on its LEFT side; shaders use
`M * v`. Citro3D uses `GPU_CULL_BACK_CCW` with the mirror removed:
**UNVERIFIED on hardware** (P6).

**Sky.** `CameraMath.View` builds the sky's clip-to-direction as
`Invert(CreateLookAt(Zero, camera.Forward, camera.Up) * projection)` — the follow
camera's OWN axes, because the view recovers forward through a cancellation that
changes the ULP. `Matrix4x4.Invert` follows .NET's SSE shuffle/FMA sequence and
`Vector4.Dot`'s pairwise reduction, not a cofactor formula
(`mdk_linux/src/render/system_numerics_inverse.cpp`); exact high-cancellation
oracle bits are pinned by `PASS SystemNumericsMatrixInverse`.

**Depth layers and the depth pull.** `computeArenaDepthLayers` is the shared half
of the C# `Layers.Of`: rounded plane keys, biggest-first stacking, and a smaller
triangle one layer above the highest bigger one it overlaps. The layer joins the
batch key and `MeshDraw::material.depth_layer`. `render/depth_pull.cpp` is the
C# `DepthPull.Of`; Vulkan applies `world * depth_pull * view * projection`.
`tests/scripts/layer_tests.cpp` pins the C# cases; forcing every draw nearer
changes 0.009% (max 58), proving the path is live. `renderer_c3d.cpp` does not
apply the pull — see `OPEN_ITEMS.md`.

**Frame golden.** `tests/frames/reference.sha256` pins level 3, 320x240, seed 1,
frame 1: `oracle=b60e72552c40…` — re-captured this batch and byte-identical to the
first capture, so the reference has never moved — `cpp=58615fc85d02…` (regression
baseline, NOT a fidelity reference), gap `bad_percent=1.240 max_delta=247
mean_delta=0.3895` at tolerance 0. The gate checks determinism, byte-exact match
to `cpp=`, and that the oracle gap does not exceed the ceiling. The ceiling may
only shrink.

**Kurt.** `game/kurt_sprite.{h,cpp}` is the C#'s `Kurt/KurtSprite.cs`: a frame is
a world quad at the feet, one sprite pixel = 1/360 of the view height, hotspot 101
px above the feet, drawn as two row bands — the body pulled in front of the floors
the eye's rays to its 3x3 sample points cross (`ArenaSpace::floorCrossing`), the
rows around the feet pulled two world units nearer still. `Pick` selects the
animation and frame (loop/once wrap, the chute's K_CHUTE 0-4 then K_CHUTEC's sway,
K_CHUTE frame 7 once on `chuteClosing`). `Player` now carries the C#'s visual state
machine (`UpdateState`/`Animate`/`UpdateSpecial`/`UpdateDeath`/`SetState`/`RunRate`)
so `state()`, `animationFrame()` and `chuteClosing()` exist; a hard landing enters
K_CRASHL and owns the step, a dead Kurt keeps falling until he lands. The states
another subsystem owns (firing, knock-down, slide, ledges, ride, beta) have no
writer and their oracle branches are taken as they are false — see OPEN_ITEMS.

**Samplers.** The C# has TWO (`Renderer.cs:249-250`), bound per resource in
`DrawOne`: index texture REPEAT, palette CLAMP, sky panorama both CLAMP. Arena UVs
are texels normalised by texture size, so a surface showing a texture twice has a
coordinate above 1 and the original TILES it. Vulkan has `sampler` (clamp) and
`samplerRepeat`, and `getDescriptorSet` is keyed by (texture, palette, repeat).

**HUD.** The canvas is 360 units high and `360 * target_width / target_height`
wide (C# `Renderer.CanvasWidth`). The frontend invents no inventory slots or
target bar, the blink advances on the C#'s 30 Hz boundary, and a dead player's
skull uses the C#'s 60/s fade to 255. It still respawns in place because the
menu/save/level-end flow is not ported.

**Frontend.** Linux loads CMI/sound bank, runs `ScriptRuntime` after Kurt and
before camera, and rebuilds it on respawn. The follow camera consumes
`cameraTrackPitch` while its request lives, otherwise current arena pitch.
Coverage: C# 238, C++ 213 labels / 212 unique / 211 true dispatches, 27
remaining; 16 partial, 195 complete.

**Verified on this tree:** `make -C tests/scripts clean && make -C tests/scripts`
exited 0 ending `All cases match the oracle.` — no movement golden moved, the
walk/footing/pickup cases still match the oracle. `cmake --build build-linux
--target mdk -j16` exited 0. `bash tools/check.sh` exited 0 (`CHECK: ALL GATES
PASSED`, SMDH SKIPPED for no CIA, `gates_frames PASS` with
`L3_f1.bmp sha=58615fc85d02e009`). `DEVKITPRO=/opt/devkitpro
DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds` exited 0 and built
`mdk.3dsx` (69 RomFS files, 79M staged). `tests/frames/capture_oracle.sh`
re-captured the oracle on `:99` in the private namespace and reproduced
`b60e72552c40…` exactly. **No VM golden changed and no emulator or Old-3DS
hardware run was performed.** Runtime combat/inventory `Kurt` remains separate
from movement `Player`; object rendering, sub-step frame blending and the 3DS
frontend binding stay deferred.

**Next increment: P5 the sky band.** It is 3.023% of the frame's pixels and every
one of its deltas is under 99 — scattered, not structural — so start by
characterising it (which rows, whether the delta correlates with the panorama's
row or with the fade) before changing the sky path. Then the 8x5 Kurt patch at
x 174-181, y 127-131 (max 247): a fully-wrong detail on one sprite frame, most
likely a hotspot or a frame-index difference, not a shading one.

**Do not "fix" `follow_camera.cpp`'s `normalize`.** It multiplies by a reciprocal
where `Vector3.Normalize` reads as a division. Making it a division REGRESSES the
sky from 3.023% to 5.730% — the reciprocal form is what the oracle matches.
Measured, not assumed.

**The 3DS sampler fix stays deferred with cause** — the 3DS pads index textures to
POT with transparent padding, so a plain `C3D_TexSetWrap(REPEAT)` would tile at the
padded period and sample discarded padding. The full reasoning is in `OPEN_ITEMS.md`.

## Standing open items

The full list with its evidence is `docs/OPEN_ITEMS.md`. The ones that bite:

- **Frame record (owner directive 2026-10-29, in the tree)**: a batch that changes
  what the renderer draws runs `tools/publish_frame.sh <label>` after the gates and
  commits `docs/frames/<date>-<label>.png` + `.txt` WITH the batch. The oracle's
  pixels stay out. Iteration 30 published `2026-10-30-kurt-sprite` beside the
  3.323% -> 1.240% movement; `2026-10-30-depth-pull` before it was byte-identical.
- **C3D depth state still differs from the C#** (test disabled vs LESS_OR_EQUAL
  test/write), which is also why `renderer_c3d.cpp` ignores `depth_layer`.
- **The oracle capture environment is not simplifiable** (glslang flags, `NUGET_PACKAGES`,
  the `unshare` namespace for `Xvfb :99`) — the three requirements are listed in
  `OPEN_ITEMS.md`, with `/tmp/kilo` readable but not writable by this harness.
- Reviews 24/25/27/28 had no BLOCKER or MAJOR; review-29 had none either. Its two
  MINORs (the frame manifest's empty gap line, and lavapipe pinning) and its NIT are
  listed in `OPEN_ITEMS.md`; review-25's descriptor-cache MINOR is too.

## Verification commands

```
make -C tests/scripts clean && make -C tests/scripts
bash tools/check.sh
DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds
python3 tools/runner/gates_frames.py        # gate 7 on its own
tests/frames/regenerate_reference.sh        # only for an intentional frame change
```

Tests do not track header dependencies: run `make -C tests/scripts clean` after
header changes. Keep the monolithic `ScriptVm::execute()` switch. Never weaken a
gate or test.
