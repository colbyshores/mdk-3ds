# Phase 2 continuation checkpoint

Last updated: 2026-10-31 (batch 9: the **Runner** is ported — `ScriptRuntime::
motionRunner` is `ObjectBehaviors.UpdateRunner` (0x43daf4's `SW_H150` branch), the
hamster that idles on `H150_I` and runs from Kurt. That was the last known
byte-divergence in any object dump: at L7 t1000 the oracle had the hamster on `H150_I`
frame 36 and the port had no animation at all, and the idle chance is drawn from the
**same `Random`** every scripted random decision takes, so the port was running a
different random stream from level 7 tick 329. `ScriptRuntime` now takes `TRAVSPRT.BNI`
the way the C# constructor does, because the runner's animations come out of it
(`Items.GetAnimation`). Goldens are now **438 PASS / 0 FAIL / 0 SKIP** (was 432):
`L7_t1000_{pose,transform,layout,draw,stream_bound}` plus `L3_t1200_blend`, every one
copied from the oracle probe. The pinned frame is byte-unchanged, measured: level 3 has
no hamster. Persistent handoff — keep it under ~150 lines; batch narrative goes in
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
6. **DONE — the object pass, all the way to the GPU, and review-3/-5/-6's findings on
   it.** `ObjectView::addObject` is `ObjectView.Draw` in the oracle's order; `game/
   object_draw.{h,cpp}` is `Renderer.QueueStream` + `Renderer.Draw`; `render/
   surface_cache.{h,cpp}` is the resolver `LevelView` shares; `mdk_linux/src/main.cpp`
   runs the C#'s `DrawObjects`; `render/draw_queue.{h,cpp}` submits the frame's draws in
   the oracle's pass order; the transform each draw carries is the interpolated one.
   Measured at the oracle's `--wait=1` frame: `1.188 / max 175 / mean 0.1405` with the
   pass, `1.251 / 231 / 0.1910` with it skipped.
   **NEXT, in priority order:** (a) the C3D depth and blend state still differ from the
   C# (Standing open items) and the Citro3D backend ignores `MeshDraw::world`, so the
   3DS cannot place an object until it has a per-draw matrix; (b) the middle 173 px of
   the pinned frame, which is **not** the object pass: at frame 1 neither tree has a
   script object, so that patch is something else and is still open. The 1.240% gap is
   three unrelated things and only one is portable — sky 774 px / 3.023% / max 99 is
   shader-codegen in `clip_to_direction`'s ill-conditioned w row (do NOT re-litigate the
   panorama, palette, uniforms, projection or camera); middle 173 px / 0.676% / max 247
   is one 8x5 patch at x 174-181, y 127-131 that is not Kurt and is not an object
   either; bottom 5 px. (c) **DONE — `ObjectMotion.Bank` (batch 8) and the Runner (this
   batch)**: what held the object goldens at t1, and what left L7 t1000 diverging.
   (d) The next numeric slice: `FrameBlend.Lerp` is `Vector3.Lerp` and the port's
   `from + (to-from)*t` is not its rounding — the oracle's is `fmaf(from, 1-t, to*t)`
   (probed 400 triples: 187 / 96 / 0 mismatches), costing 1 ULP on one object at L3 t120.
   `fmaf` is a `bl fmaf` libm call on devkitARM at `-mfpu=vfp`, and the current spelling
   contracts to one `vmla.f32` per component **whatever `-ffp-contract` says** (VFPv2
   VMLA; the Makefile measures the flag changing nothing), so this needs a target codegen
   decision, not a substitution. Evidence in `docs/OPEN_ITEMS.md`.

Deferred with cause: 248 `push_kurt` awaits knock-back; Rides writes the alarm
countdown; consumers of 135/195/202/203/81 remain partial; the 3DS sampler fix and
Kurt's muzzle flash, ride pose and footsteps; `Vector3.Lerp`'s rounding (above); the
other half of `FrameBlend` (Kurt's blended feet/yaw,
`Record/Update/Turn/KurtAt`) and `TransformOf`'s driven-carrier shift, both with
`Rides`; the runner's RUN branch and any **picture** of the runner (no camera looks at
the hamster — measured). All with evidence in `OPEN_ITEMS.md`.
Old-3DS timing is UNMEASURED.

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
`MdkCapabilities::sprite_residency_bytes` (0 on the desktop, 1 MiB on the 3DS) —
UNVERIFIED on hardware (P6). `Player::fall` is `Kurt.Fall` whole.
**Object pass (place, pose, layout, roll, bank, queue, GPU, blend, behaviours).**
`MdkObject::transform()` is `CreateScale * (rolling ? RollingBasis : Rotation) *
CreateTranslation(pos + z*lift)`, in that order, via `mdk::multiply` (System.Numerics
element arithmetic: one rounded product + three `fma`). `pose_parts()` is `PoseParts`,
from caches keyed by model address, invalidated by `scripts::forgetPoseCaches()` and
`MdkObject::forget_poses()`. `game/object_view.{h,cpp}` is `ObjectView`, keyed by
`(model, palette)`; the stream refuses a batch that would pass `kObjectStreamCapacity`
(`1 << 18`, the C#'s `Renderer.StreamVertices`). The GPU half is
`game/object_draw.{h,cpp}` (ONE dynamic mesh, uploaded once per frame, one draw per
queued batch at its offset). **The transform a frame draws with** is
`transform_at(alpha)`: `remember()` snapshots place/angles/arena at the top of each
tick, and `blends()` refuses to interpolate across `alpha >= 1`, a missing snapshot, an
arena change or a jump (`frame_blend::jumped` is `MaxSpeed * seconds` squared, so a
teleport is drawn where it ends, never on the way). `rotation()` is
`rotation_of(roll, pitch, yaw)`; the blended angles go through the same libm trig and
the same `@vfp-exempt R6` pin. **`roll` has a writer** (batch 8): `motionBank` is
`ObjectMotion.Bank`, run per object per tick between `update_rolling` and
`previous_position = position`, where the C# calls `Bank`. **The built-in behaviours
have the Runner**: `motionRunner` is `ObjectBehaviors.UpdateRunner`, called from
`motionPickup` where the C# calls it — after the chute has gone (`attached == nullptr`),
before the Still-list spin — and its `H150_R` / `H150_I` come from `itemsAnimation`, the
C#'s `Items.GetAnimation`, parsed out of the sprites archive the runtime now borrows
(`tick1` and the Linux frontend both load `TRAVSPRT.BNI` and pass it). The idle chance
is `rng().next(32768) < 72`, drawn **only** when the object is idle or its animation is
done: that is the oracle's stream, not an approximation of it.

**Draw submission: order and lifetime.** `mdk_render_draw_mesh` **queues**; the backend
submits the frame's draws bucketed by `Pass` — Solid, DoubleSided, Mirror, Blended,
OnTop, Overlay, queue order inside a bucket — `Renderer.cs:1000-1004, 1049-1057`,
shared as `render/draw_queue.{h,cpp}` because the order is the oracle's, not a
backend's; the flush point is the end of the frame's 3D scene. Lifetime (AGENTS.md P3):
a Vulkan dynamic mesh has one buffer per frame in flight and writes the slot its own
frame draws from; a released mesh or a resized canvas ring is **retired** and destroyed
two frames out; the 3DS backend defers `linearFree` to the next `C3D_FrameBegin`.

**Rolling (`ObjectMotion.Roll`).** `MdkObject::update_rolling()` turns the basis about
the horizontal axis across the tick's movement, and `runTick` calls it **before**
`previous_position = position`, where the C# does. `mdk::create_from_axis_angle` is
**bit-exact against .NET 10** because .NET builds that matrix from a quaternion; the
look-alike element-wise spelling differs by 2-8 ULP per element.
## Verified on this tree

- `make -C tests/scripts -j8` exited 0, ended `All cases match the oracle.`, **438 PASS
  lines** (was 432: `L7_t1000_{pose,transform,layout,draw,stream_bound}` and
  `L3_t1200_blend`), no FAIL, no SKIP, **no existing golden moved**. Every new golden was
  copied from the C# oracle probe (`mdkprobe <level> <ticks> 1 <mode>`) after diffing it
  against the port's own dump; all six are byte-identical to it. Independently re-checked:
  L3 t120/t1200, L4/L5/L6 t120, L7 t1200/t30000 and L8 t1 `transform` dumps are still
  byte-identical to the oracle.
- `bash tools/check.sh` exited 0 (`CHECK: ALL GATES PASSED`; SMDH SKIPPED for no CIA;
  `gates_frames PASS`, `L3_f1.bmp sha=58615fc85d02e009` — byte-identical to the pinned
  `cpp=`, so `reference.sha256` was NOT regenerated). No gate needed a pin or a
  relaxation: `gates_vfp` blockers=0, `audit_c_with_classes` 0, `audit_r11_idiv` no
  candidates, `audit_arm11_math` clean, `gates_core_parity` clean.
- `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds -j8`
  exited 0 and built `mdk.3dsx`; `cmake --build build-linux --target mdk -j8` exited 0.
- `DISPLAY=:99 tools/publish_frame.sh hamster-runner` wrote the pinned pair (`1.240 /
  max 247 / mean 0.1819`), byte-identical to the seven records before it: level 3 has no
  `SW_H150`, so nothing runs. The sidecar says why and records the level-7 with/without
  measurement. No emulator and no Old-3DS hardware run was performed.

**Do not "fix" `follow_camera.cpp`'s `normalize`** (it multiplies by a reciprocal where
`Vector3.Normalize` reads as a division; making it a division REGRESSES the sky from
3.023% to 5.730%), and the 3DS sampler fix stays deferred with cause (the 3DS pads
index textures to POT with transparent padding, so a plain `C3D_TexSetWrap(REPEAT)`
would tile at the padded period). Both with evidence in `docs/OPEN_ITEMS.md`.

## Standing open items

Full list with evidence in `docs/OPEN_ITEMS.md`; the frame-record rule is in
`docs/frames/README.md` and the "the pin cannot show this" rule in
`docs/evidence/README.md`. The ones that bite: **C3D depth and blend state still differ
from the C#** (test disabled vs LESS_OR_EQUAL test/write; `Pass::Blended` glass is
opaque), which is why `renderer_c3d.cpp` ignores `depth_layer` — and non-`Solid` batches
are in **10 of level 3's 17** arenas; the Citro3D backend ignores `MeshDraw::world`, so
the 3DS cannot place an object; the sprite residency bound is compile-verified only (the
3DS frontend binds no Kurt); `depth_pull.cpp` is `DepthPull.Of` — Vulkan applies the
pull, the C3D backend does not; the oracle capture environment is not simplifiable
(glslang flags, `NUGET_PACKAGES`, the `unshare` namespace for `Xvfb :99`), and
`capture_oracle.sh` is pinned to frame 1, so a late-frame oracle picture (level 7, say)
needs owner-authorized tooling.

## Verification commands

```
make -C tests/scripts clean && make -C tests/scripts   # goldens
tests/scripts/build/tick1 dump draw 3 1                # the queued draws (also `bound`)
tests/scripts/build/tick1 dump blend 3 1               # the blended transforms (also `blendcases`)
tests/scripts/build/tick1 dump transform 7 1000        # the hamster's animation
bash tools/check.sh                                    # all gates
DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds
cmake --build build-linux --target mdk -j8             # the Linux frontend
python3 tools/runner/gates_frames.py                   # gate 7 alone
tests/frames/regenerate_reference.sh                   # intentional frame change only
```

Tests do not track header dependencies: run `make -C tests/scripts clean` after
header changes. Never weaken a gate or test.
