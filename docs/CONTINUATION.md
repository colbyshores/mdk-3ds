# Phase 2 continuation checkpoint

Risk: medium — removes a defaulted constructor argument from `ScriptRuntime` (an
internal interface, ~47 mechanical call sites) and adds two goldens plus a harness dump
mode; no shipped golden moved, the pinned frame is byte-unchanged, all gates green.

Last updated: 2026-11-01 (batch 10: the Runner's **running** half is pinned and the
record around it is corrected. `golden/L7_t120_runner_{a,b}.txt` dump the hamster's
pos/yaw/anim/frame/push/flags after EVERY one of 120 ticks with Kurt placed inside
`DANT_1` through a live `Player` — it was unpinned because no shipped start puts Kurt
within `RunnerStart` (20) of it, and both goldens are byte-identical to the C# probe's
new `runner` mode. Review-2's M1 was right too: a runner picture was never impossible —
the previous batch measured at `--at` z=0, where Kurt has no floor; at z=9 the scene
renders, so `docs/evidence/2026-11-01-L7-runner-{scene,hamster-visible}.png` exist.
`ScriptRuntime`'s `sprites` argument is now REQUIRED, as the C#'s is: a runtime without
the archive runs a different random stream. Goldens **440 PASS / 0 FAIL / 0 SKIP** (was
438); the pinned frame is byte-unchanged, measured. Keep this file under ~150 lines;
batch narrative goes in `docs/PROGRESS_LOG.md`.

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
   the pinned frame, which is **not** the object pass — at frame 1 neither tree has a
   script object, so that patch is something else and is still open. The sky / middle /
   bottom split of the 1.240% gap, with its measurements and the "do NOT re-litigate the
   panorama, palette, uniforms, projection or camera" note, is in `docs/OPEN_ITEMS.md`.
   (c) **DONE — `ObjectMotion.Bank` (batch 8) and the Runner (batches 9-10)**: what held
   the object goldens at t1, and what left L7 t1000 diverging.
   (d) The next numeric slice: `FrameBlend.Lerp` is `Vector3.Lerp` and the port's
   `from + (to-from)*t` is not its rounding — the oracle's is `fmaf(from, 1-t, to*t)`,
   costing 1 ULP on one object at L3 t120. `fmaf` is a `bl fmaf` libm call on devkitARM
   at `-mfpu=vfp`, and the current spelling contracts to one `vmla.f32` per component
   **whatever `-ffp-contract` says**, so this needs a target codegen decision, not a
   substitution. Probe and evidence in `docs/OPEN_ITEMS.md`.

Deferred with cause: 248 `push_kurt` awaits knock-back; Rides writes the alarm
countdown; consumers of 135/195/202/203/81 remain partial; the 3DS sampler fix and
Kurt's muzzle flash, ride pose and footsteps; `Vector3.Lerp`'s rounding (above); the
other half of `FrameBlend` (Kurt's blended feet/yaw,
`Record/Update/Turn/KurtAt`) and `TransformOf`'s driven-carrier shift, both with
`Rides`. All with evidence in `OPEN_ITEMS.md`.
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
done: that is the oracle's stream, not an approximation of it. **`sprites` is a REQUIRED
constructor argument** (as in the C#): without the archive there is no `H150_R`/`H150_I`
and, because the idle draw is guarded on the animation, the random stream differs — so
omission is a compile error and a harness that means "no archive" passes `nullptr` out
loud. Both halves are pinned: the idle by `L7_t1000_*`, the run by `L7_t120_runner_{a,b}`.

**Draw submission: order and lifetime.** `mdk_render_draw_mesh` **queues**; the backend
submits the frame's draws bucketed by `Pass` — Solid, DoubleSided, Mirror, Blended,
OnTop, Overlay, queue order inside a bucket — `Renderer.cs:1000-1004, 1049-1057`,
shared as `render/draw_queue.{h,cpp}` because the order is the oracle's, not a
backend's; the flush point is the end of the frame's 3D scene. Lifetime (AGENTS.md P3)
is in `render/draw_queue.h`: a Vulkan dynamic mesh writes the slot its own frame draws
from and a released mesh is destroyed two frames out; the 3DS defers `linearFree` to the
next `C3D_FrameBegin`.

**Rolling (`ObjectMotion.Roll`).** `MdkObject::update_rolling()` turns the basis about
the horizontal axis across the tick's movement, and `runTick` calls it **before**
`previous_position = position`, where the C# does. `mdk::create_from_axis_angle` is
**bit-exact against .NET 10** because .NET builds that matrix from a quaternion; the
look-alike element-wise spelling differs by 2-8 ULP per element.
## Verified on this tree

All three gates were re-run from a clean invocation on this tree, and the two new goldens
were re-generated from the C# oracle independently of the batch that wrote them — see
`docs/PROGRESS_LOG.md`, "Verification actually run for this batch".

- `make -C tests/scripts -j8` exited 0, ended `All cases match the oracle.`, **440 PASS
  / 0 FAIL / 0 SKIP** (was 438: `L7_t120_runner_{a,b}`), **no existing golden moved**.
  Both new goldens are byte-identical to the oracle probe (`mdkprobe 7 120 1 runner
  <x,y,z>`, a scratch build of the repository's own `src/`; only the three print-only
  instrumented files differ from `src/`).
- `bash tools/check.sh` exited 0 — `CHECK: ALL GATES PASSED`, SMDH SKIPPED for no CIA,
  `gates_frames PASS` (`L3_f1.bmp sha=58615fc85d02e009`, byte-identical to the pinned
  `cpp=`, so `reference.sha256` was NOT regenerated), `gates_vfp` blockers=0,
  `audit_c_with_classes` 0, `audit_r11_idiv` no candidates, `audit_arm11_math` clean,
  `gates_core_parity` clean, `gates_batch_integrity PASS`. No gate needed a pin or a
  relaxation.
- `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds -j8`
  exited 0 and built `mdk.3dsx`; `cmake --build build-linux --target mdk -j8` exited 0.
- The pinned pair (`1.240 / max 247 / mean 0.1819`) is byte-identical to its fifteen
  predecessors: level 3 has no `SW_H150`, so nothing runs. The two `docs/evidence/`
  pairs (level 7, frame 30, `DISPLAY=:99`, lavapipe) measure `0.422 / 324 / 58 /
  0.0358` and `1.298 / 997 / 87 / 0.1580`; both numbers were reproduced with
  `tests/frames/compare_frames.py --measure` against the captures, and the committed
  PNGs are pixel-identical to them. No emulator and no Old-3DS hardware run.

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
tests/scripts/build/tick1 dump runner 7 120 -21,-13,0.73   # the hamster's run, per tick
bash tools/check.sh                                    # all gates
DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds
cmake --build build-linux --target mdk -j8             # the Linux frontend
python3 tools/runner/gates_frames.py                   # gate 7 alone
tests/frames/regenerate_reference.sh                   # intentional frame change only
```

Tests do not track header dependencies: run `make -C tests/scripts clean` after
header changes. Never weaken a gate or test.
