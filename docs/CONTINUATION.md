# Phase 2 continuation checkpoint

Last updated: 2026-10-31 (the object pass reaches the GPU and the Linux frontend draws
objects: `render/surface_cache.{h,cpp}` is the C# resolver's texture/palette caches
factored out of `LevelView`, `game/object_draw.{h,cpp}` is `Renderer.QueueStream` +
`Renderer.Draw`, and `main.cpp` runs `Viewer.DrawObjects`. The pinned frame is
byte-unchanged — at frame 1 neither tree has a script object, so the frame reference did
not move.) The opcode coverage (211 true C++ dispatches / 238, 27 deferred) is
unchanged. Persistent handoff — keep it under ~150 lines; batch narrative goes in
`docs/PROGRESS_LOG.md`.

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
6. **DONE — the object pass, all the way to the GPU, and review-3's findings on it.**
   `ObjectView::addObject` is `ObjectView.Draw` in the oracle's order, filling the
   frame's `ObjectStream` with corners and one `ObjectStreamDraw` per batch (offset,
   corners, surface, palette, animated frame, transform), pinned by
   `golden/L3_t1_draw.txt` and `golden/L3_t1_stream_bound.txt`. The GPU half is
   `game/object_draw.{h,cpp}` (one dynamic mesh, uploaded once per frame, one draw per
   queued batch at its offset) and the surface/palette mapping is
   `render/surface_cache.{h,cpp}`, which `LevelView` shares instead of duplicating;
   `mdk_linux/src/main.cpp` runs the C#'s `DrawObjects`. **Measured** at the oracle's
   `--wait=1` frame (60 fixed steps): the gap is `1.188 / max 175 / mean 0.1405` with
   the pass and `1.251 / 231 / 0.1910` with it skipped, so drawing objects moves the
   picture towards the oracle.
   **NEXT for the renderer, in priority order:** (a) the C# buckets its queued commands
   by `Pass` before rendering (`Renderer.cs:1004, 1049-1057`) and both C++ backends
   record draws as they come, so a `Blended` object now draws in queue order instead of
   after every `Solid`; (b) the C3D depth and blend state still differ from the C#
   (Standing open items) and non-`Solid` batches are in **10 of level 3's 17** arenas;
   (c) the middle 173 px of the pinned frame, which is **not** the object pass: at
   frame 1 neither tree has a script object, so that patch is something else and is
   still open. The 1.240% gap is three unrelated things and only one is portable —
   sky 774 px / 3.023% / max 99 is shader-codegen in `clip_to_direction`'s
   ill-conditioned w row (do NOT re-litigate the panorama, palette, uniforms,
   projection or camera); middle 173 px / 0.676% / max 247 is one 8x5 patch at
   x 174-181, y 127-131 that is not Kurt and is not an object either; bottom 5 px.

Deferred with cause: 248 `push_kurt` awaits knock-back; Rides writes the alarm
countdown; consumers of 135/195/202/203/81 remain partial; the 3DS sampler fix and
Kurt's muzzle flash, ride pose and footsteps; `ObjectMotion.Bank`, so the port never
writes `roll` from motion (the reason the transform golden is pinned at t1 only); the
level-7 `SW_H150` animation, which the oracle has running (`H150_I` frame 36) at t1000
and the port never starts — a pose/animation item. Both with evidence in
`OPEN_ITEMS.md`. Old-3DS timing is UNMEASURED.

## Current tree state (VERIFIED)

**Camera and renderer.** `buildLookAtCamera` uses the C#'s RIGHT-HANDED
`CreateLookAt` basis and perspective projection. Vulkan folds clip.y into the combined
matrix; the sky inverse carries the flip on its LEFT side; shaders use `M * v`.
Citro3D uses `GPU_CULL_BACK_CCW` with the mirror removed and culls per draw (Solid
culls back, the other passes do not), the mode cached in the **Citro3D context**. **The
3DS result is UNVERIFIED on hardware** (P6).

**Frame golden.** `tests/frames/reference.sha256` pins level 3, 320x240, seed 1,
frame 1: `oracle=b60e72552c40…`, `cpp=58615fc85d02…` (a regression baseline, NOT a
fidelity reference), gap `1.240 / 247 / 0.1819` at tolerance 0: determinism,
byte-exactness to `cpp=`, and a ceiling that may only shrink.

**Kurt, sprites, death.** `game/kurt_sprite.{h,cpp}` is `Kurt/KurtSprite.cs` (world
quad at the feet, one sprite pixel = 1/360 of the view height, hotspot 101 px up, two
row bands at the oracle's floor crossings). The port keeps the sprite frames' palette
indices (5.30 MiB for `TRAVSPRT.BNI`, measured) and uploads on first draw under
`MdkCapabilities::sprite_residency_bytes` (0 on the desktop, 1 MiB on the 3DS), which
drops the least-recently-drawn frames and re-makes an evicted one. `Player::fall` is
`Kurt.Fall` whole (`FallMove`, then the 50-unit fall-out test).

**Object pass (place, pose, layout, roll, queue, GPU).**
`MdkObject::transform()` is `CreateScale * (rolling ? RollingBasis : Rotation) *
CreateTranslation(pos + z*lift)`, in that order, via `mdk::multiply` (System.Numerics
element arithmetic: one rounded product + three `fma`). `pose_parts()` is `PoseParts`:
rest vertices or `bake(model, animation)[frame]`, from caches keyed by model address,
invalidated by `scripts::forgetPoseCaches()` at the top of `LevelData::load` and by
`MdkObject::forget_poses()`. `game/object_view.{h,cpp}` is `ObjectView`: the layout
stage (batches by surface in first-seen order, per-part corner and first-triangle
tables, UVs scaled by the texture's own size, `arrange`, the corner count) and
`addObject`, which fills the frame's `ObjectStream`. The layout is keyed by
`(model, palette)` — the C#'s key — which is why the queued draw can name the palette
from the look. The stream grows to the frame (2559 corners at L3 t1, 5616 at L8 t1) but
refuses a batch that would pass `kObjectStreamCapacity` (`1 << 18`, the C#'s
`Renderer.StreamVertices`), leaving the cursor for a later smaller batch.
`game/object_draw.{h,cpp}` uploads it once per frame into ONE dynamic mesh and issues
the queued draws at their offsets; `render/surface_cache.{h,cpp}` maps a
surface+palette to a backend texture/palette and is shared with `LevelView`, as the C#
gives both views one resolver. The Linux frontend runs the C#'s `DrawObjects` between
`view.draw()` and Kurt's sprite.

**Rolling (`ObjectMotion.Roll`).** `MdkObject::update_rolling()` turns the basis about
the horizontal axis across the tick's movement, and `runTick` calls it **before**
`previous_position = position`, where the C# does. `mdk::create_from_axis_angle` is
**bit-exact against .NET 10** because .NET builds that matrix from a quaternion
(sin/cos of the HALF angle, then the `1-(yy+zz) / xy+wz / xz-wy …` conversion); the
look-alike element-wise axis-angle spelling differs by 2-8 ULP per element. Its
`sinf`/`cosf` carry an `@vfp-exempt R6` pin: the LUT is not bit-identical to
`MathF.Sin` and the basis is gameplay.

## Verified on this tree

- `make -C tests/scripts clean && make -C tests/scripts -j8` exited 0, ended `All
  cases match the oracle.`, **421 PASS lines**, no FAIL, no SKIP, **no golden moved**.
  The two new files are not in the tests' source list, exactly as `level_view.cpp` is
  not: they call the renderer seam, which no host test links.
- `bash tools/check.sh` exited 0 (`ALL GATES PASSED`; SMDH SKIPPED for no CIA;
  `gates_frames PASS`, `L3_f1.bmp sha=58615fc85d02e009` — byte-identical to the pinned
  `cpp=`, so `reference.sha256` was NOT regenerated). No gate needed a pin or a
  relaxation: the new per-frame work is integer indexing, pointer copies and one
  `memcpy`, so R2/R3/R4/R6/R11 have nothing to bite on, and
  `audit_c_with_classes.py` reports 0 violations.
- `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds -j8`
  exited 0 and built `mdk.3dsx` (69 RomFS files, 79M staged).
  `cmake --build build-linux --target mdk -j8` exited 0.
- **The object pass, measured.** `tests/frames/capture_oracle.sh` re-ran and
  reproduced the pinned oracle frame byte for byte (`b60e72552c40…`), which is what makes
  the second number trustworthy. At the oracle's `--wait=1` frame (60 fixed steps,
  level 3, 320x240, display `:99` in the private namespace) against the C++ at frame 60
  (`DISPLAY=:99`, headless): with the pass `1.188 / max 175 / mean 0.1405`, with it
  skipped `1.251 / 231 / 0.1910`, and the pass alone changes 0.081% of the frame. At
  frame 1 it draws NOTHING in either tree — `ScriptRuntime` ticks at 1/30 and frame 1
  is 1/60 s — which is why the pinned frame did not move.
- `DISPLAY=:99 tools/publish_frame.sh objects-on-gpu` wrote the pair with
  `bad_percent=1.240 max_delta=247 mean_delta=0.1819`; the PNG is byte-identical to the
  previous batch's, for the frame-1 reason above. No emulator and no Old-3DS hardware
  run was performed.

**Do not "fix" `follow_camera.cpp`'s `normalize`.** It multiplies by a reciprocal
where `Vector3.Normalize` reads as a division; making it a division REGRESSES the sky
from 3.023% to 5.730%. (The rolling `orthonormal` is the opposite case: there the
oracle divides, and it is pinned bit-for-bit.)

**The 3DS sampler fix stays deferred with cause** — the 3DS pads index textures to POT
with transparent padding, so a plain `C3D_TexSetWrap(REPEAT)` would tile at the padded
period and sample discarded padding.

## Standing open items

Full list with evidence in `docs/OPEN_ITEMS.md`; the frame-record rule is in
`docs/frames/README.md`. The ones that bite: **C3D depth and blend state still differ
from the C#** (test disabled vs LESS_OR_EQUAL test/write; `Pass::Blended` glass is
opaque), which is why `renderer_c3d.cpp` ignores `depth_layer` — and non-`Solid`
batches are in **10 of level 3's 17** arenas; the sprite residency bound is
compile-verified only (the 3DS frontend binds no Kurt); `depth_pull.cpp` is
`DepthPull.Of` — Vulkan applies the pull, the C3D backend does not; the oracle capture
environment is not simplifiable (glslang flags, `NUGET_PACKAGES`, the `unshare`
namespace for `Xvfb :99`).

## Verification commands

```
make -C tests/scripts clean && make -C tests/scripts   # goldens
tests/scripts/build/tick1 dump draw 3 1                # the queued draws (also `bound`)
bash tools/check.sh                                    # all gates
DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds
cmake --build build-linux --target mdk -j8             # the Linux frontend
python3 tools/runner/gates_frames.py                   # gate 7 alone
tests/frames/regenerate_reference.sh                   # intentional frame change only
```

Tests do not track header dependencies: run `make -C tests/scripts clean` after
header changes. Never weaken a gate or test.
