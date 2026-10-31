# Phase 2 continuation checkpoint

Last updated: 2026-10-31 (review-1's findings are closed: the rolling basis is
advanced, the pose caches are invalidated, the layout stage has its own golden and
`tick1` fails instead of skipping). The pinned frame and the opcode coverage
(211 true C++ dispatches / 238, 27 deferred) are unchanged. Persistent handoff —
keep it under ~150 lines; batch narrative goes in `docs/PROGRESS_LOG.md`.

## PRIORITY ORDER (owner directive 2026-10-25)

1. **DONE — mixer call-sites** (`ObjectMotion.cs:133`, `ObjectBehaviors.cs:90-115`).
2. **DONE — `Debris.Shatter` + opcodes 137/138/139**, dump line and oracle goldens.
   BreakUp/Trail/Updraft await Effects/Fans.
3. **DONE for the C# oracle — opcode conversion.** 211 dispatches are pinned in
   `docs/OPCODE_COVERAGE.md`; the Linux frontend runs `ScriptRuntime` around the live
   `Player` and feeds the camera. The remaining 27 each need a deferred subsystem;
   do not blind-port them or read them off `MDKD3D.EXE`.
4. **DONE — P5.0 frame-golden harness** (`gates_frames.py` is gate 7; proven to fail
   on a seeded regression).
5. **DONE — review-30's 3DS texture-residency MAJOR.** Sprite frames are decoded at
   load and uploaded on first draw under a named 1 MiB budget; the Citro3D eviction
   keeps the source and the draw path restores an evicted texture.
6. **DONE — the object pass's stage 1** (place, pose, layout) **and its review
   findings**: `ObjectMotion.Roll` is ported and driven, the rest/bake caches are
   invalidated on level load, and the layout stage has an oracle golden of its own
   (`golden/L3_t1_layout.txt`) beside `golden/rolling_basis.txt`.
   **NEXT: the draw stage** — posed corners into a per-frame vertex stream, one draw
   per batch with the object's transform, `Pass::DoubleSided`, the
   `texture_frame`/`effect_frames` inputs, and the frontend wiring; the two
   candidate stream designs and why the C#'s one-buffer-per-frame needs a seam
   change are in `docs/OPEN_ITEMS.md`. The 1.240% gap is three unrelated things and
   only one is portable: **sky, 774 px / 3.023% / max 99** is shader-codegen in
   `clip_to_direction`'s ill-conditioned w row, NOT an input mismatch — do not
   re-litigate the panorama, palette, uniforms, projection or camera. **Middle, 173
   px / 0.676% / max 247**: every delta above 128 is one 8x5 patch at x 174-181,
   y 127-131, and it is not Kurt (the C++'s decoded `K_STILL` frame 0 is
   byte-identical to the oracle's and does not cover those pixels), so `max_delta`
   cannot move until the object pass draws. **Bottom: 5 px**.

Deferred with cause: 248 `push_kurt` awaits knock-back; Rides writes the alarm
countdown; consumers of 135/195/202/203/81 remain partial; the 3DS sampler fix and
Kurt's muzzle flash, ride pose and footsteps; `ObjectMotion.Bank`, so the port never
writes `roll` from motion (measured in `OPEN_ITEMS.md`, and the reason the transform
golden is pinned at t1 only). Old-3DS timing is UNMEASURED.

## Current tree state (VERIFIED)

**Camera and renderer.** `buildLookAtCamera` uses the C#'s RIGHT-HANDED
`CreateLookAt` basis and perspective projection. Vulkan folds clip.y into the
combined matrix; the sky inverse carries the flip on its LEFT side; shaders use
`M * v`. Citro3D uses `GPU_CULL_BACK_CCW` with the mirror removed and culls per draw
(Solid culls back, the other passes do not), with the mode cached in the **Citro3D
context** so repeated Solid draws do not reflash the effect block. **The 3DS result
is UNVERIFIED on hardware** (P6).

**Sky and depth.** The sky inverse uses the follow camera's own axes and .NET's
exact matrix-inverse reduction (`PASS SystemNumericsMatrixInverse`);
`computeArenaDepthLayers` and `render/depth_pull.cpp` are `Layers.Of` and
`DepthPull.Of` — Vulkan applies the pull, the C3D backend does not.

**Frame golden.** `tests/frames/reference.sha256` pins level 3, 320x240, seed 1,
frame 1: `oracle=b60e72552c40…`, `cpp=58615fc85d02…` (a regression baseline, NOT a
fidelity reference), gap `bad_percent=1.240 max_delta=247 mean_delta=0.1819` at
tolerance 0. The gate checks determinism, byte-exactness to `cpp=` and a ceiling that
may only shrink; a batch that changes the picture goes through `regenerate_reference.sh`.

**Kurt, sprites, death.** `game/kurt_sprite.{h,cpp}` is `Kurt/KurtSprite.cs` (world
quad at the feet, one sprite pixel = 1/360 of the view height, hotspot 101 px up, two
row bands at the oracle's floor crossings). The port keeps the sprite frames' palette
indices (5.30 MiB for `TRAVSPRT.BNI`, measured) and uploads on first draw;
`MdkCapabilities::sprite_residency_bytes` is 0 on the desktop and 1 MiB on the 3DS,
the cache drops the least-recently-drawn frames and the draw path re-makes an evicted
one. `Player::fall` is `Kurt.Fall` whole (`FallMove`, then the 50-unit fall-out test);
below the arena the C#'s post-move test sets `OnFloor` true again in the same step.

**Object pass, stage 1 (place, pose, layout, roll).** `MdkObject::transform()` is
`CreateScale * (rolling ? RollingBasis : Rotation) * CreateTranslation(pos + z*lift)`,
in that order, via `mdk::multiply` (System.Numerics element arithmetic: one rounded
product + three `fma`). `pose_parts()` is `PoseParts`: rest vertices or
`bake(model, animation)[frame]`, from caches shared by every object and keyed by
model address. **Those caches are now invalidated**: `scripts::forgetPoseCaches()`
(`scripts/pose_cache.h`, dependency-free so the game layer need not include the
scripts) runs at the top of `LevelData::load` — the C#'s `Viewer.cs:185` `RestPoses`
clear — and `MdkObject::forget_poses()` calls it too. `game/object_view.{h,cpp}` is
the layout stage: batches by surface in first-seen order, per-part corner and
first-triangle tables, UVs scaled by the texture's own size, `arrange` and the corner
count. Nothing draws it yet — see the priority order.

**Rolling (`ObjectMotion.Roll`).** `MdkObject::update_rolling()` turns the basis
about the horizontal axis across the tick's movement, and `runTick` calls it
**before** `previous_position = position` — both now live at the end of the
per-object motion block, not inside `motionVelocity`, where the write happened twice
and made every roll read a zero movement. `mdk::orthonormal` is Gram-Schmidt with
`Vector3.Normalize`'s divide-by-length. `mdk::create_from_axis_angle` is **bit-exact
against .NET 10** because .NET builds that matrix from a quaternion (sin/cos of the
HALF angle, then the `1-(yy+zz) / xy+wz / xz-wy …` conversion); the look-alike
element-wise axis-angle spelling differs by 2-8 ULP per element, enough to move the
basis at the sixth decimal after a dozen ticks. Its `sinf`/`cosf` carry an
`@vfp-exempt R6` pin: the LUT is not bit-identical to `MathF.Sin` and the basis is
gameplay.

## Verified on this tree

- `make -C tests/scripts clean && make -C tests/scripts -j8` exited 0, ended
  `All cases match the oracle.`, **419 PASS lines**, no FAIL, no SKIP. New:
  `PASS L3_t1_layout`, `PASS rolling_basis`; no existing golden moved. Both new
  goldens ARE the probe's output (a probe built against a copy of `src/Mdk.Game`
  under `/tmp/mdk-pi/obj3`; the repository's `src/Mdk.Game/**` was not touched) and
  the C++ dumps reproduce them byte for byte.
- `bash tools/check.sh` exited 0 (`ALL GATES PASSED`; SMDH SKIPPED for no CIA;
  `gates_frames PASS`, `L3_f1.bmp sha=58615fc85d02e009`). `create_from_axis_angle`'s
  trig needed the R6 pin to keep `audit_arm11_math` clean — the gate is right and the
  pin records why it does not apply here.
- `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds -j8`
  exited 0 and built `mdk.3dsx` (69 RomFS files, 79M staged).
- `DISPLAY=:99 tools/publish_frame.sh rolling-basis-layout` wrote the pair with
  `bad_percent=1.240 max_delta=247 mean_delta=0.1819`; the PNG is byte-identical to
  the previous batch's (no render-path file changed). No emulator and no Old-3DS
  hardware run was performed.

**Do not "fix" `follow_camera.cpp`'s `normalize`.** It multiplies by a reciprocal
where `Vector3.Normalize` reads as a division; making it a division REGRESSES the
sky from 3.023% to 5.730%. (The rolling `orthonormal` is the opposite case: there
the oracle divides, and it is pinned bit-for-bit.)

**The 3DS sampler fix stays deferred with cause** — the 3DS pads index textures to
POT with transparent padding, so a plain `C3D_TexSetWrap(REPEAT)` would tile at the
padded period and sample discarded padding.

## Standing open items

Full list with evidence in `docs/OPEN_ITEMS.md`; the frame-record rule is in
`docs/frames/README.md`. The ones that bite: **C3D depth and blend state still
differ from the C#** (test disabled vs LESS_OR_EQUAL test/write; `Pass::Blended`
glass is opaque), which is why `renderer_c3d.cpp` ignores `depth_layer` — and
non-`Solid` batches are in **10 of level 3's 17** arenas (review-4's correction);
the sprite residency bound is compile-verified only (the 3DS frontend binds no
Kurt), and `KurtSprite::load()` leaks its palette and mesh if called twice; the
oracle capture environment is not simplifiable (glslang flags, `NUGET_PACKAGES`, the
`unshare` namespace for `Xvfb :99`).

## Verification commands

```
make -C tests/scripts clean && make -C tests/scripts   # goldens
tests/scripts/build/tick1 dump layout 3 1              # a dump, to diff vs the probe
bash tools/check.sh                                    # all gates
DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds
python3 tools/runner/gates_frames.py                   # gate 7 alone
tests/frames/regenerate_reference.sh                   # intentional frame change only
```

Tests do not track header dependencies: run `make -C tests/scripts clean` after
header changes. Never weaken a gate or test.
