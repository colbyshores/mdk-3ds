# Phase 2 continuation checkpoint

Last updated: 2026-10-29 (review-27/28 numerical fidelity). Opcode coverage is
unchanged: 211 true C++ dispatches / 238, 27 deferred. The frame gap remains
**3.323%**; the pairwise dot and saturating float-to-int boundary fixes do not
move the pinned frame.

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
5. **P5 — close the measured 3.323% frame gap**, one renderer slice at a time. The
   largest remaining band is the **ground and HUD** (bottom third: 3.754% of its
   pixels differ, mean 3.5272, max 255 — the largest mean delta in the frame). The
   sky band is 3.023% and its residual is sparse scattered pixels, not structure.

Deferred with cause: 248 `push_kurt` awaits knock-back; Rides writes the alarm
countdown; consumers of 135/195/202/203/81 remain partial; the 3DS half of the
sampler fix (below). Old-3DS timing remains UNMEASURED.

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
`tests/scripts/layer_tests.cpp` pins the C# cases. Cold cost is 5.8–24 ms per
level on x86 `-O2` (worst arena 10.3 ms), once per arena build. The pinned frame
is byte-identical, but forcing every draw nearer changes 0.009% (max 58), proving
the path is live. `renderer_c3d.cpp` does not apply the pull — see `OPEN_ITEMS.md`.

**Frame golden.** `tests/frames/reference.sha256` pins level 3, 320x240, seed 1,
frame 1: `oracle=b60e72552c40…` (unchanged since first captured),
`cpp=3d67efbeab85…` (regression baseline, NOT a fidelity reference), gap
`bad_percent=3.323 max_delta=255 mean_delta=1.3783` at tolerance 0. The gate checks
determinism, byte-exact match to `cpp=`, and that the oracle gap does not exceed
the ceiling. The ceiling may only shrink.

**Samplers.** The C# has TWO (`Renderer.cs:249-250`), bound per resource in
`DrawOne`: index texture REPEAT, palette CLAMP, sky panorama both CLAMP. Arena UVs
are texels normalised by texture size, so a surface showing a texture twice has a
coordinate above 1 and the original TILES it. Vulkan has `sampler` (clamp) and
`samplerRepeat`, and `getDescriptorSet` is keyed by (texture, palette, repeat):
mesh and canvas ask for repeat, the sky for clamp.

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

**Verified on this tree:** a clean `tests/scripts` rebuild and the subsequent full
`make -C tests/scripts` exited 0, ending `All cases match the oracle.` with
`PASS SystemNumericsMatrixInverse` and `PASS DepthLayersAndDepthPull`.
`cmake --build build-linux --target mdk -j8` exited 0. `bash tools/check.sh`
exited 0 (`CHECK: ALL GATES PASSED`, SMDH SKIPPED for no CIA, `gates_frames PASS`
with `L3_f1.bmp sha=3d67efbeab85c8a4`). The devkitARM `make -C mdk_3ds` exited
0 and built `mdk.3dsx` (69 RomFS files, 79M). No oracle re-capture was needed:
the reference did not move. **No VM golden changed and no emulator or Old-3DS
hardware run was performed.** Runtime combat/inventory `Kurt` remains separate
from movement `Player`; object rendering and 3DS frontend binding stay deferred.

**Next increment: P5 ground and HUD.** The bottom third is still the largest band
(3.754% bad, mean 3.5272, max 255); its max 255 says something is fully wrong at
some pixels, not shaded differently. Separate the HUD overlay from the ground
shading before changing either, then take the middle band (3.191% / mean 1.5197 /
max 247).

**Do not "fix" `follow_camera.cpp`'s `normalize`.** It multiplies by a reciprocal
where `Vector3.Normalize` reads as a division. Making it a division REGRESSES the
sky from 3.023% to 5.730% — the reciprocal form is what the oracle matches.
Measured, not assumed.

**The 3DS sampler fix stays deferred with cause.** `renderer_c3d.cpp` never calls
`C3D_TexSetWrap`, so its wrap field is 0 — not `GPU_REPEAT` (`enums.h:39`) — and it
clamps like Vulkan used to. It cannot take the same one-liner: the 3DS pads index
textures to POT with TRANSPARENT padding and rescales UVs by real/padded
(`renderer_c3d.cpp:1133-1147`), so a plain REPEAT tiles at the PADDED period and
samples index-0 padding, which discards — holes, not tiling. Needs the padding
filled with the tile pattern; unverifiable without hardware.

## Standing open items

The full list with its evidence is `docs/OPEN_ITEMS.md`. The ones that bite:

- **Frame record (owner directive 2026-10-29, now in the tree)**: a batch that
  changes what the renderer draws MUST run `tools/publish_frame.sh <label>` after
  the gates and commit `docs/frames/<date>-<label>.png` + `.txt` WITH the batch
  (`ralph/builder.md`, `docs/frames/README.md`). The oracle's pixels stay out.
  Iteration 28 published `2026-10-30-depth-pull` and its frame is byte-identical
  to the baseline, so the gap beside it is unchanged at 3.323%.
- **C3D depth state still differs from the C#** (test disabled vs LESS_OR_EQUAL
  test/write), which is also why `renderer_c3d.cpp` ignores `depth_layer`.
- **The oracle capture environment is not simplifiable**: glslang needs `-D` and
  an attached `-I<dir>`, `NUGET_PACKAGES` must reach `ppy.SDL3-CS`, and `Xvfb :99`
  must run inside `unshare --user --map-root-user -m` with a private
  `/tmp/.X11-unix`. `/tmp/kilo` is readable but not writable by this harness.
- Reviews 24/25/27/28 had no BLOCKER or MAJOR. Review-27's pairwise-dot MINOR
  and review-28's saturating-bound MINOR are closed in `66707dd`; review-25's
  descriptor-cache MINOR remains listed in `OPEN_ITEMS.md`.

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
