# Phase 2 continuation checkpoint

Last updated: 2026-10-31 (the object pass's stage 1 — place, pose and layout — is
ported and pinned; the draw stage is next). The pinned frame is unchanged. Opcode
coverage is unchanged: 211 true C++ dispatches / 238, 27 deferred.

This is persistent handoff. Keep it under ~150 lines; batch narrative goes in
`docs/PROGRESS_LOG.md`.

## PRIORITY ORDER (owner directive 2026-10-25)

1. **DONE — mixer call-sites** (`ObjectMotion.cs:133`, `ObjectBehaviors.cs:90-115`).
2. **DONE — `Debris.Shatter` + opcodes 137/138/139**, dump line and oracle goldens.
   BreakUp/Trail/Updraft await Effects/Fans.
3. **DONE for the C# oracle — opcode conversion.** 211 dispatches are pinned in
   `docs/OPCODE_COVERAGE.md`; the Linux frontend runs `ScriptRuntime` around the
   live `Player` and feeds the camera. The remaining 27 each need a deferred
   subsystem; do not blind-port them or reverse-engineer them from `MDKD3D.EXE`.
4. **DONE — P5.0 frame-golden harness** (`gates_frames.py` is gate 7; proven to
   fail on a seeded regression).
5. **DONE — review-30's 3DS texture-residency MAJOR.** The sprite frames are
   decoded at load and uploaded on first draw; the 3DS names a 1 MiB residency
   budget and the shared cache keeps the least-recently-drawn frames inside it;
   the Citro3D eviction keeps the source and the draw path restores an evicted
   texture, so no arena surface can stay untextured.
6. **IN PROGRESS — the object pass (`ObjectView`).** Stage 1 landed: the object's
   place and pose (`MdkObject::transform()`, `pose_parts()`, the shared rest/bake
   caches) and the layout stage (`game/object_view.{h,cpp}`: batches by surface,
   the per-part corner and first-triangle tables, `arrange`, the corner count),
   pinned by `golden/L3_t{1,120,1200}_pose.txt` and `golden/L3_t1_transform.txt`
   — which is the first golden that pins the **baked animation pose** at all.
   **NEXT: the draw stage** (posed corners into a per-frame vertex stream, one
   draw per batch with the object's transform, `Pass::DoubleSided`, the
   `texture_frame`/`effect_frames` inputs) and the frontend wiring; the two
   candidate stream designs and why the C#'s one-buffer-per-frame needs a seam
   change are written up in `docs/OPEN_ITEMS.md`. The 1.240% gap is three
   unrelated things and only one is portable: **sky, 774 px / 3.023% / max 99**
   is shader-codegen in `clip_to_direction`'s ill-conditioned w row, NOT an input
   mismatch — do not re-litigate the panorama, palette, uniforms, projection or
   camera for it. **Middle, 173 px / 0.676% / max 247**: every delta above 128 is
   one 8x5 patch at x 174-181, y 127-131, and it is not Kurt — the C++'s decoded
   `K_STILL` frame 0 is byte-identical to the oracle's and does not cover those
   pixels, so `max_delta` cannot move until the object pass draws. The other 133
   are scattered edge pixels. **Bottom: 5 px**, effectively exact.

Deferred with cause: 248 `push_kurt` awaits knock-back; Rides writes the alarm
countdown; consumers of 135/195/202/203/81 remain partial; the 3DS sampler fix
and Kurt's muzzle flash, ride pose and footsteps; `ObjectMotion.Bank`, so the
port never writes `roll` from motion (measured in `OPEN_ITEMS.md`, and the reason
the transform golden is pinned at t1 only). Old-3DS timing is UNMEASURED.

## Current tree state (VERIFIED)

**Camera and renderer.** `buildLookAtCamera` uses the C#'s RIGHT-HANDED
`CreateLookAt` basis and perspective projection. Vulkan folds clip.y into the
combined matrix; the sky inverse carries the flip on its LEFT side; shaders use
`M * v`. Citro3D uses `GPU_CULL_BACK_CCW` with the mirror removed, and selects
culling per draw: Solid culls back faces; DoubleSided and the other non-solid
passes do not. The installed mode is cached in the **Citro3D context** (global,
not per render target), so repeated Solid draws no longer dirty and reflash the
effect block on every mesh. **The 3DS result is UNVERIFIED on hardware** (P6).

**Sky and depth.** The sky inverse uses the follow camera's own axes and .NET's
exact matrix-inverse reduction, pinned by `PASS SystemNumericsMatrixInverse`.
`computeArenaDepthLayers` and `render/depth_pull.cpp` are the C# `Layers.Of` and
`DepthPull.Of`; Vulkan applies the pull and the C3D backend does not.

**Frame golden.** `tests/frames/reference.sha256` pins level 3, 320x240, seed 1,
frame 1: `oracle=b60e72552c40…` (byte-identical across captures) and
`cpp=58615fc85d02…` (regression baseline, NOT a fidelity reference), gap
`bad_percent=1.240 max_delta=247 mean_delta=0.1819` at tolerance 0. The gate
checks determinism, byte-exact match to `cpp=`, and that the gap stays inside the
ceiling, which may only shrink — so a batch that changes the picture must be run
through `regenerate_reference.sh`, which re-measures the gap into the diff.

**Kurt.** `game/kurt_sprite.{h,cpp}` is the C#'s `Kurt/KurtSprite.cs`: a world quad
at the feet, one sprite pixel = 1/360 of the view height, hotspot 101 px above the
feet, drawn as two row bands with the oracle's floor crossings; `Player` carries
the C#'s visual state machine.

**Sprite and texture residency.** The C# uploads every sprite frame at level load
and keeps it for ever. The port decodes the frames, keeps the **palette indices**
(5.30 MiB for `TRAVSPRT.BNI`'s 19 animations / 360 frames, measured) and uploads
a frame the first time it is drawn. `MdkCapabilities::sprite_residency_bytes`
names the bound: 0 on the desktop (the C#'s own behaviour), 1 MiB on the 3DS; the
shared cache drops the least-recently-drawn frames over it, never the one being
drawn (`PASS SpriteResidencyPolicy`; a port policy, so no C# twin). On the 3DS
every texture keeps its source, so the LRU eviction is **restorable** (the draw
path re-makes an evicted texture rather than leaving the surface untextured), and
a texture the GPU may still be reading is released at the next `C3D_FrameBegin`.

**Death and `Fall`.** `Player::fall` is the C#'s `Kurt.Fall` whole (`FallMove`,
then the 50-unit fall-out test), called from `UpdateVertical`, `UpdateSpecial`
and `UpdateDeath`. Below the arena the C#'s **post-move** fall-out assignment sets
`OnFloor` true again in the same step; the real divergences were `OnFloor` **and**
`VerticalSpeed` parity.

**Object pass, stage 1 (place, pose, layout).** `MdkObject::transform()` is the
C#'s `CreateScale * (rolling ? RollingBasis : Rotation) * CreateTranslation(pos +
z*lift)` chain, in that order, using `mdk::multiply` — the System.Numerics element
arithmetic (one rounded product + three `fma`) that `rotation()` already spelled
out, now in `util/matrix.h` instead of a local lambda. `pose_parts()` is the C#'s
`PoseParts`: the rest vertices or `bake(model, animation)[frame]`, from caches
shared by every object (the C#'s `RestPoses`/`Bake`) keyed by model address and
released by `forget_poses()`. `game/object_view.{h,cpp}` is the layout stage:
batches by surface in first-seen order, per-part corner and first-triangle tables,
UVs scaled by the texture's own size (zero for a flat surface), `arrange`
(insertion sort by first visible triangle) and the per-batch corner count.
`MaterialResolver::resolve` took the C#'s `Pass` argument (arena Solid, objects
DoubleSided; glass and mirrors keep their own pass either way). Nothing draws the
layout yet — see the priority order.

**Samplers.** The C# has TWO (`Renderer.cs:249-250`): index texture REPEAT, palette
CLAMP, sky both CLAMP, so arena UVs above 1 tile as in the oracle. Vulkan keys its
descriptor set by (texture, palette, repeat), and a release or an update drops the
cached sets that name the destroyed view.

**HUD and frontend.** The canvas is 360 units high and the C#'s aspect-derived
width; menu/save/level-end flow is not ported. Linux loads CMI/sound bank, runs
`ScriptRuntime` after Kurt and before camera, and rebuilds it on respawn.
Coverage: C# 238, C++ 211 dispatches, 27 deferred; 16 partial, 195 complete.

## Verified on this tree

- `make -C tests/scripts -j8` exited 0, ended `All cases match the oracle.`; the
  new object cases printed `PASS L3_t1_pose`, `PASS L3_t120_pose`,
  `PASS L3_t1200_pose`, `PASS L3_t1_transform`. No existing golden moved.
  `cmake --build build-linux --target mdk -j8` exited 0.
- `bash tools/check.sh` exited 0 (`CHECK: ALL GATES PASSED`; SMDH SKIPPED for no
  CIA; `gates_frames PASS` with `L3_f1.bmp sha=58615fc85d02e009`; batch integrity
  PASS). The frame is byte-unchanged: this batch touched no draw path.
- `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds -j8`
  exited 0, compiled `build/object_view.o` (the 3DS glob picks up `game/*.cpp`) and
  built `mdk.3dsx` (69 RomFS files, 79M staged).
- `DISPLAY=:99 tools/publish_frame.sh object-pose-layout` wrote
  `docs/frames/2026-10-31-object-pose-layout.png` + `.txt` with
  `bad_percent=1.240 max_delta=247 mean_delta=0.1819` — byte-identical to the
  previous C++ frame, as expected: no render-path file changed.
- The object goldens came from a probe built against a **pristine copy** of
  `src/Mdk.Game` under `/tmp/mdk-pi/obj1`. The repository's `src/Mdk.Game/**` was
  not touched. No emulator or Old-3DS hardware run was performed.
**Do not "fix" `follow_camera.cpp`'s `normalize`.** It multiplies by a reciprocal
where `Vector3.Normalize` reads as a division; making it a division REGRESSES the
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
  LESS_OR_EQUAL test/write; `Pass::Blended` glass is opaque), which is why
  `renderer_c3d.cpp` ignores `depth_layer`. Non-`Solid` batches are in **10 of
  level 3's 17** arenas (review-4's correction).
- The sprite residency bound is compile-verified only: the 3DS frontend binds no
  Kurt, so neither the 1 MiB bound nor the ~5.3 MiB of kept index sources has been
  measured, and `KurtSprite::load()` leaks its palette and mesh if called twice.
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
header changes. Never weaken a gate or test.
