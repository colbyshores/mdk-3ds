# Phase 2 continuation checkpoint

Last updated: 2026-10-29 (arena depth layers + depth pull). Opcode coverage is
unchanged: 211 true C++ dispatches / 238, 27 deferred. The frame gap is unchanged
at **3.323%** — the depth pull moves no pixel in the pinned frame (proved below).

This is persistent handoff. Keep it under ~150 lines; batch narrative goes in
`docs/PROGRESS_LOG.md`.

## PRIORITY ORDER (owner directive 2026-10-25)

1. **DONE — mixer call-sites** (`ObjectMotion.cs:133`, `ObjectBehaviors.cs:90-115`).
2. **DONE — `Debris.Shatter` + opcodes 137/138/139**, dump line and oracle goldens.
   BreakUp/Trail/Updraft await Effects/Fans.
3. **DONE for the C# oracle — opcode conversion.** 211 dispatches implemented and
   pinned in `docs/OPCODE_COVERAGE.md`. The Linux frontend builds `ScriptRuntime`,
   binds the live `Player`, runs its 30 Hz clock after each 60 Hz player step, and
   feeds `camera_track`/the current arena to the camera. The remaining 27 each need
   a deferred subsystem. Do not blind-port them or reverse-engineer them from
   `MDKD3D.EXE` (owner directive 2026-10-28).
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
`CreateLookAt` basis and `CreatePerspectiveFieldOfView` projection (the old
left-handed build mirrored x/z and culled front faces). Vulkan folds clip.y into
the combined matrix (`flipClipY`); the sky inverse carries the flip on its LEFT
side; shaders use `M * v`; canvas ortho is written straight for the y-down
framebuffer. Citro3D uses `GPU_CULL_BACK_CCW` with the mirror removed:
**UNVERIFIED on hardware** (P6).

**Sky.** `CameraMath.View` builds the sky's clip-to-direction as
`Invert(CreateLookAt(Zero, camera.Forward, camera.Up) * projection)` — the follow
camera's OWN axes, not the view matrix's, because the view recovers forward as
`normalize((position+forward)-position)` and that cancellation changes the ULP;
`buildFollowCamera*` therefore store the exact `followCameraOrient` axes on
`Camera::forward`/`up`. `Matrix4x4.Invert` is translated mechanically from .NET's
SSE shuffle/`MultiplyAddEstimate` sequence, not a cofactor formula
(`mdk_linux/src/render/system_numerics_inverse.cpp`; `M*M^-1 = I` to 2.4e-7,
singular rejected). Both halves contribute; ablation in `PROGRESS_LOG`.

**Depth layers and the depth pull.** `computeArenaDepthLayers`
(`render/arena_scene.cpp`) is the shared geometry half of the C# `Layers.Of`: planes
keyed by a rounded normal and plane distance, stacked biggest-first, a smaller
triangle one layer above the highest bigger one it overlaps (Sutherland-Hodgman in
the plane's own 2D basis). The batch run-break takes the layer as well as material
and group, and `MeshDraw::material.depth_layer` carries it.
`render/depth_pull.cpp` is the C# `DepthPull.Of` (1/256 of the eye distance per
layer, 1/128 more for lines, as `CreateScale(1-share, eye)` with translation
`eye*(1-scale)`), and `renderer_vk.cpp` applies `world * depth_pull * view *
projection` — the C# `Renderer.cs:1072` chain. Checked by
`tests/scripts/layer_tests.cpp` (the C# `LayerTests.cs` + `DepthPullTests.cs`
cases). Cold: 5.8–24 ms per LEVEL (all arenas) on x86 `-O2`, worst single arena
10.3 ms (L4 `MEAT_5`), once per arena build. The pinned frame is byte-identical
because draw order under `LESS_OR_EQUAL` already resolves its coplanar details;
forcing every draw one layer nearer changes 0.009% of the frame (max 58), which is
what proves the path is live. `renderer_c3d.cpp` does not apply the pull — see
`docs/OPEN_ITEMS.md`.

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

**HUD.** The seam exposes `mdk_render_canvas_width`: the canvas is 360 units high
and `360 * target_width / target_height` wide (C# `Renderer.CanvasWidth`). The
frontend invents no inventory slots and no target bar (a fresh C# level has an
empty inventory and `GetBar() == (0, 0)`), the blink advances on the C#'s 30 Hz
boundary, and a dead player's skull uses the C#'s 60/s fade to 255. It still
respawns in place because menu/save/level-end flow is not ported.

**Frontend.** Linux loads CMI/sound bank, runs `ScriptRuntime` after Kurt and
before camera, and rebuilds it on respawn. The follow camera consumes
`cameraTrackPitch` while its request lives, otherwise current arena pitch. Capture
CLI: `--headless --fixed-step --capture= --capture-frame=N --size=WxH --level=N
--at=x,y,z[,yaw]`. Coverage: C# 238, C++ 213 labels / 212 unique / 211 true
dispatches, 27 remaining; 16 partial, 195 complete.

**Verified on this tree:** `make -C tests/scripts` exited 0 ending `All cases match
the oracle.` with `PASS DepthLayersAndDepthPull`; `bash tools/check.sh` exited 0
(`CHECK: ALL GATES PASSED`, SMDH SKIPPED for no CIA, `gates_frames PASS` with
`L3_f1.bmp sha=3d67efbeab85c8a4`); the devkitARM `make -C mdk_3ds` exited 0 and
built `mdk.3dsx` (69 RomFS files, 79M); `cmake --build build-linux --target mdk`
exited 0 and gate 7 re-run against that binary reproduced the pinned hash. No
oracle re-capture was needed (the reference did not move); the last one, iteration
26, ran on display `:99` in a private namespace (lavapipe/llvmpipe, `Level 3: 17
arenas, 9708 triangles`, `Kurt at -4.00 0.00 192.98 yaw 96`). **No VM golden
changed and no emulator or Old-3DS hardware run was performed.** Runtime
combat/inventory `Kurt` remains separate from movement `Player`; object rendering
and 3DS frontend binding stay deferred. C++ uses one seeded legacy RNG per process,
unlike C#'s fresh unseeded generator. Review-21 caveat stands: post-camera solid
refresh pins only the current arena, not C#'s `SolidArenas`.

**Next increment: review-27's MINOR, then P5 ground and HUD.** First the
reviewer's verified fix in `mdk_linux/src/render/system_numerics_inverse.cpp`:
`dot()` reduces sequentially where .NET's `Vector4.Dot` reduces pairwise, so
`Matrix4x4.Invert` is 1–2 ULP off on high-cancellation inputs (bit-exact on all
1305 realistic camera matrices, so no current player symptom). Make it
`s0=p0+p1; s1=p2+p3; return s0+s1;` and soften the header's "mechanical
translation" claim; the frame hash must not move. Then the bottom third is still
the largest band (3.754% bad, mean 3.5272, max 255); its max 255 says something is
fully wrong at some pixels, not shaded differently. Separate the HUD overlay from
the ground shading before changing either, then take the middle band (3.191% /
mean 1.5197 / max 247).

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

- **An owner directive landed mid-batch, still untracked**: `tools/publish_frame.sh`
  and `docs/frames/README.md` (written 2026-10-29 by the other loop sharing this
  checkout) say every batch that MOVES THE PICTURE leaves a PNG of the pinned
  frame in `docs/frames/` beside its measured gap; the oracle's pixels stay out.
  Neither file is committed yet, so nothing was published here — and iteration 28
  moved no pixels anyway (its frame is byte-identical to the baseline). See
  `docs/OPEN_ITEMS.md`.
- **C3D depth state still differs from the C#** (test disabled vs LESS_OR_EQUAL
  test/write), which is also why `renderer_c3d.cpp` ignores `depth_layer`.
- **The oracle capture environment is not simplifiable**: glslang needs `-D` and
  an attached `-I<dir>`, `NUGET_PACKAGES` must reach `ppy.SDL3-CS`, and `Xvfb :99`
  must run inside `unshare --user --map-root-user -m` with a private
  `/tmp/.X11-unix`. `/tmp/kilo` is readable but not writable by this harness.
- Reviews 24/25/27 had no BLOCKER or MAJOR; review-27's MINOR is the next
  increment (above) and review-25's descriptor-cache MINOR is listed there.

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
