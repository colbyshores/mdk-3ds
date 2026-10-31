# Phase 2 continuation checkpoint

Last updated: 2026-10-31 (the object pass's DRAW stage is ported up to the GPU and
pinned by `golden/L3_t1_draw.txt`; it is not wired to a frontend yet). The pinned
frame and the opcode coverage (211 true C++ dispatches / 238, 27 deferred) are
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
6. **DONE — the object pass's stage 1** (place, pose, layout) **and its review
   findings**: `ObjectMotion.Roll` is ported and driven, the rest/bake caches are
   invalidated on level load, and the layout stage has an oracle golden of its own
   (`golden/L3_t1_layout.txt`) beside `golden/rolling_basis.txt`.
   **DONE — the draw stage's geometry half**: `ObjectView::addObject` is
   `ObjectView.Draw` — arrange, copy the posed corners onto the frame's
   `ObjectStream`, queue one draw per batch at its stream offset with the
   `texture_frame`/`effect_frames` inputs and the transform — pinned by
   `golden/L3_t1_draw.txt` and identical to the oracle on levels 3–8 except the
   known `Bank` transform divergence.
   **NEXT: the GPU half and the wiring** — map each batch's `Surface` to a backend
   texture/palette (factor `LevelView`'s `textureId`/`flatColourId` out rather than
   copying it), upload the stream once per frame through
   `mdk_render_create_dynamic_mesh`/`update_mesh` (no seam change is needed), issue
   the draws, call it from `mdk_linux/src/main.cpp` after `view.draw()`, and only
   then regenerate the frame reference: the picture changes, so
   `tests/frames/reference.sha256` must be rewritten by `regenerate_reference.sh`
   and nothing else. The whole recipe is in `docs/OPEN_ITEMS.md`.
   The 1.240% gap is three unrelated things and only one is portable — sky 774 px /
   3.023% / max 99 is shader-codegen in `clip_to_direction`'s ill-conditioned w row
   (do NOT re-litigate the panorama, palette, uniforms, projection or camera);
   middle 173 px / 0.676% / max 247 is one 8x5 patch at x 174-181, y 127-131 that
   is not Kurt and cannot move until the object pass draws; bottom 5 px. Bands and
   evidence: `docs/OPEN_ITEMS.md`.

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
(Solid culls back, the other passes do not), the mode cached in the **Citro3D
context** so repeated Solid draws do not reflash the effect block. **The 3DS result
is UNVERIFIED on hardware** (P6).

**Frame golden.** `tests/frames/reference.sha256` pins level 3, 320x240, seed 1,
frame 1: `oracle=b60e72552c40…`, `cpp=58615fc85d02…` (a regression baseline, NOT a
fidelity reference), gap `bad_percent=1.240 max_delta=247 mean_delta=0.1819` at
tolerance 0. The gate checks determinism, byte-exactness to `cpp=` and a ceiling that
may only shrink.

**Kurt, sprites, death.** `game/kurt_sprite.{h,cpp}` is `Kurt/KurtSprite.cs` (world
quad at the feet, one sprite pixel = 1/360 of the view height, hotspot 101 px up, two
row bands at the oracle's floor crossings). The port keeps the sprite frames' palette
indices (5.30 MiB for `TRAVSPRT.BNI`, measured) and uploads on first draw under
`MdkCapabilities::sprite_residency_bytes` (0 on the desktop, 1 MiB on the 3DS), which
drops the least-recently-drawn frames and re-makes an evicted one. `Player::fall` is
`Kurt.Fall` whole (`FallMove`, then the 50-unit fall-out test); below the arena the
C#'s post-move test sets `OnFloor` true again in the same step.

**Object pass (place, pose, layout, roll, and the draw stage up to the GPU).**
`MdkObject::transform()` is `CreateScale * (rolling ? RollingBasis : Rotation) *
CreateTranslation(pos + z*lift)`, in that order, via `mdk::multiply` (System.Numerics
element arithmetic: one rounded product + three `fma`). `pose_parts()` is `PoseParts`:
rest vertices or `bake(model, animation)[frame]`, from caches shared by every object
keyed by model address, invalidated by `scripts::forgetPoseCaches()` at the top of
`LevelData::load` (the C#'s `Viewer.cs:185` `RestPoses` clear) and by
`MdkObject::forget_poses()`. `game/object_view.{h,cpp}` is `ObjectView`: the layout
stage (batches by surface in first-seen order, per-part corner and first-triangle
tables, UVs scaled by the texture's own size, `arrange`, the corner count) and now
`addObject`, which is `ObjectView.Draw` — the model/dead/layout guards, `PoseParts`,
the hidden mask, `TextureFrame >= 0 ? TextureFrame : 0`, `EffectFrames > 0 ->
Glow.Shines`, `Arrange`, `Visible`, the per-part corner copy loop and `Shine`, in the
oracle's order. It appends the corners to the frame's `ObjectStream` and queues one
`ObjectStreamDraw` per batch (stream offset, surface, animated frame, transform) —
the C#'s `Renderer.Stream` + `Renderer.Draw` pair, minus the GPU. **Nothing uploads
or draws yet** (see the priority order), so the pinned frame is unchanged; the stream
grows to the frame instead of carrying the C#'s 1<<18 bound (measured on the oracle:
2559 corners at L3 t1, 5616 at L8 t1).

**Rolling (`ObjectMotion.Roll`).** `MdkObject::update_rolling()` turns the basis
about the horizontal axis across the tick's movement, and `runTick` calls it
**before** `previous_position = position` — both at the end of the per-object motion
block, where the C# does (inside `motionVelocity` the write happened twice and every
roll read a zero movement). `mdk::orthonormal` is Gram-Schmidt with
`Vector3.Normalize`'s divide-by-length. `mdk::create_from_axis_angle` is **bit-exact
against .NET 10** because .NET builds that matrix from a quaternion (sin/cos of the
HALF angle, then the `1-(yy+zz) / xy+wz / xz-wy …` conversion); the look-alike
element-wise axis-angle spelling differs by 2-8 ULP per element, enough to move the
basis at the sixth decimal after a dozen ticks. Its `sinf`/`cosf` carry an
`@vfp-exempt R6` pin: the LUT is not bit-identical to `MathF.Sin` and the basis is
gameplay.

## Verified on this tree

- `make -C tests/scripts -j8` exited 0, ended `All cases match the oracle.`, **420
  PASS lines**, no FAIL, no SKIP. New: `PASS L3_t1_draw`; no existing golden moved.
  The new golden IS the probe's output (the same probe tree under `/tmp/mdk-pi/obj3`,
  with `DumpDraw` added to a COPY of `ObjectView.cs`; the repository's
  `src/Mdk.Game/**` was not touched) and `tick1 dump draw` reproduces it byte for
  byte. Cross-checked beyond the pinned case: levels 4-7 at t1 and levels 3 at
  t120/t1200 differ from the oracle ONLY in `T` (transform) lines, i.e. only in the
  documented `Bank` roll; every corner, offset, surface, frame and hash matches.
- `bash tools/check.sh` exited 0 (`ALL GATES PASSED`; SMDH SKIPPED for no CIA;
  `gates_frames PASS`, `L3_f1.bmp sha=58615fc85d02e009`). No gate needed a pin or a
  relaxation this batch: the draw stage's inner loop is copies and integer indices,
  so R2/R3/R4/R6/R11 have nothing to bite on.
- `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds -j8`
  exited 0 and built `mdk.3dsx` (69 RomFS files, 79M staged).
- `DISPLAY=:99 tools/publish_frame.sh object-draw-stream` wrote the pair with
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
non-`Solid` batches are in **10 of level 3's 17** arenas; the sprite residency bound
is compile-verified only (the 3DS frontend binds no Kurt); the sky inverse needs
.NET's exact matrix-inverse reduction (`PASS SystemNumericsMatrixInverse`) and
`depth_pull.cpp` is `DepthPull.Of` — Vulkan applies the pull, the C3D backend does
not; the oracle capture environment is not simplifiable (glslang flags,
`NUGET_PACKAGES`, the `unshare` namespace for `Xvfb :99`).

## Verification commands

```
make -C tests/scripts clean && make -C tests/scripts   # goldens
tests/scripts/build/tick1 dump draw 3 1                # a dump, to diff vs the probe
bash tools/check.sh                                    # all gates
DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds
python3 tools/runner/gates_frames.py                   # gate 7 alone
tests/frames/regenerate_reference.sh                   # intentional frame change only
```

Tests do not track header dependencies: run `make -C tests/scripts clean` after
header changes. Never weaken a gate or test.
