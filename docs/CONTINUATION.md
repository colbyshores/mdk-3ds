# Phase 2 continuation checkpoint

Last updated: 2026-10-31 (the backends submit the frame's draws in the oracle's
**pass order** and a dynamic vertex buffer is no longer overwritten or freed while an
earlier frame is reading it: `render/draw_queue.{h,cpp}` is `Renderer`'s pass-bucketed
command list, shared by both backends, and the Vulkan backend's dynamic meshes and
canvas are rings with a frame-boundary retire list. The pinned frame is byte-unchanged
— measured, the reordering is a no-op at both frames that can be captured. Opcode
coverage is unchanged (211 true C++ dispatches / 238, 27 deferred). Persistent
handoff — keep it under ~150 lines; batch narrative goes in `docs/PROGRESS_LOG.md`.

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
6. **DONE — the object pass, all the way to the GPU, and review-3's and review-5's
   findings on it.** `ObjectView::addObject` is `ObjectView.Draw` in the oracle's
   order, filling the frame's `ObjectStream` with corners and one `ObjectStreamDraw`
   per batch, pinned by `golden/L3_t1_draw.txt` and `golden/L3_t1_stream_bound.txt`;
   `game/object_draw.{h,cpp}` is `Renderer.QueueStream` + `Renderer.Draw`,
   `render/surface_cache.{h,cpp}` is the resolver `LevelView` shares, and
   `mdk_linux/src/main.cpp` runs the C#'s `DrawObjects`. **Measured** at the oracle's
   `--wait=1` frame (60 fixed steps): `1.188 / max 175 / mean 0.1405` with the pass,
   `1.251 / 231 / 0.1910` with it skipped. Review-5's pass-order MAJOR is closed by
   `render/draw_queue.{h,cpp}`; its dynamic-mesh BLOCKER by the ring + retire list;
   its frame-record MAJOR by `docs/evidence/`.
   **NEXT, in priority order:** (a) **the interpolated transform** — the live frontend
   hands `addObject` `obj.transform()` where the oracle hands
   `blend.TransformOf(obj)` = `obj.TransformAt(tick)`; closing it is a slice of its
   own (the object's tick snapshot, `transform_at()`, the frontend's alpha), and it is
   review-5's remaining MAJOR; (b) the C3D depth and blend state still differ from the
   C# (Standing open items) and the Citro3D backend ignores `MeshDraw::world`, so the
   3DS cannot place an object until it has a per-draw matrix; (c) the middle 173 px of
   the pinned frame, which is **not** the object pass: at frame 1 neither tree has a
   script object, so that patch is something else and is still open. The 1.240% gap is
   three unrelated things and only one is portable — sky 774 px / 3.023% / max 99 is
   shader-codegen in `clip_to_direction`'s ill-conditioned w row (do NOT re-litigate
   the panorama, palette, uniforms, projection or camera); middle 173 px / 0.676% /
   max 247 is one 8x5 patch at x 174-181, y 127-131 that is not Kurt and is not an
   object either; bottom 5 px.

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
`MdkCapabilities::sprite_residency_bytes` (0 on the desktop, 1 MiB on the 3DS). The
3DS result is UNVERIFIED on hardware (P6). `Player::fall` is `Kurt.Fall` whole
(`FallMove`, then the 50-unit fall-out test).

**Object pass (place, pose, layout, roll, queue, GPU).**
`MdkObject::transform()` is `CreateScale * (rolling ? RollingBasis : Rotation) *
CreateTranslation(pos + z*lift)`, in that order, via `mdk::multiply` (System.Numerics
element arithmetic: one rounded product + three `fma`). `pose_parts()` is `PoseParts`:
rest vertices or `bake(model, animation)[frame]`, from caches keyed by model address,
invalidated by `scripts::forgetPoseCaches()` at the top of `LevelData::load` and by
`MdkObject::forget_poses()`. `game/object_view.{h,cpp}` is `ObjectView` (the layout
stage and `addObject`), keyed by `(model, palette)` — the C#'s key — which is why the
queued draw can name the palette from the look. The stream grows to the frame (2559
corners at L3 t1, 5616 at L8 t1) but refuses a batch that would pass
`kObjectStreamCapacity` (`1 << 18`, the C#'s `Renderer.StreamVertices`), leaving the
cursor for a later smaller batch. The GPU half is `game/object_draw.{h,cpp}` (ONE
dynamic mesh, uploaded once per frame, one draw per queued batch at its offset).

**Draw submission: order and lifetime.** `mdk_render_draw_mesh` **queues**; the backend
submits the frame's draws bucketed by `Pass` — Solid, DoubleSided, Mirror, Blended,
OnTop, Overlay, queue order inside a bucket — which is `Renderer.cs:1000-1004,
1049-1057` and is shared as `render/draw_queue.{h,cpp}` because the order is the
oracle's, not a backend's. The flush point is the end of the frame's 3D scene:
`canvas_begin`, an inset's boundary, `end_frame`. Measured with a temporary per-bucket
counter: frame 1 queues 154 Solid + 1 DoubleSided, frame 60 154 Solid + 30-33
DoubleSided, so the pinned frames are byte-identical before and after. Lifetime
(AGENTS.md P3): a Vulkan dynamic mesh has one buffer per frame in flight and writes the
slot its own frame draws from (the slot whose fence `beginFrame` waited); a released
mesh or a resized canvas ring is **retired** and destroyed at the frame boundary after
the fence wait, two frames out; the 3DS backend defers `linearFree` to the next
`C3D_FrameBegin`, the same boundary it already uses for textures.

**Rolling (`ObjectMotion.Roll`).** `MdkObject::update_rolling()` turns the basis about
the horizontal axis across the tick's movement, and `runTick` calls it **before**
`previous_position = position`, where the C# does. `mdk::create_from_axis_angle` is
**bit-exact against .NET 10** because .NET builds that matrix from a quaternion (sin/cos
of the HALF angle, then the `1-(yy+zz) / xy+wz / xz-wy …` conversion); the look-alike
element-wise axis-angle spelling differs by 2-8 ULP per element. Its `sinf`/`cosf`
carry an `@vfp-exempt R6` pin: the LUT is not bit-identical to `MathF.Sin` and the
basis is gameplay.

## Verified on this tree

- `make -C tests/scripts clean && make -C tests/scripts -j8` exited 0, ended `All
  cases match the oracle.`, **421 PASS lines**, no FAIL, no SKIP, **no golden moved**.
- `bash tools/check.sh` exited 0 (`ALL GATES PASSED`; SMDH SKIPPED for no CIA;
  `gates_frames PASS`, `L3_f1.bmp sha=58615fc85d02e009` — byte-identical to the pinned
  `cpp=`, so `reference.sha256` was NOT regenerated). No gate needed a pin or a
  relaxation: the new per-frame work is integer bucketing, a struct copy and one
  `memcpy`, so R2/R3/R4/R6/R11 have nothing to bite on, and `audit_c_with_classes.py`
  reports 0 violations. `gates_core_parity` passes with `draw_queue.cpp` in both trees.
- `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds -j8`
  exited 0 and built `mdk.3dsx` (69 RomFS files, 79M staged); `cmake --build
  build-linux --target mdk -j8` exited 0.
- **The frames, re-measured.** The oracle's frame-1 capture reproduced the pinned
  `b60e72552c40…` byte for byte, and its `--wait=1` (frame-60) capture reproduced the
  previous batch's `9ab0e0a4181b…` byte for byte — both re-run for this batch, in the
  private namespace with `Xvfb :99`. The C++ at frame 60 measures
  `1.188 / max 175 / mean 0.1405` against it and is byte-identical to the previous
  batch's frame 60 (`377b606a165e…`), which is the pass-order no-op above.
- `DISPLAY=:99 tools/publish_frame.sh pass-order-frame-safe-meshes` wrote the pinned
  pair (`bad_percent=1.240 max_delta=247 mean_delta=0.1819`, byte-identical to the
  last four records, for the reason in its own sidecar), and
  `docs/evidence/2026-10-31-L3-f60-objects.png` + `.txt` is the new versioned picture
  of the object pass. No emulator and no Old-3DS hardware run was performed.
**Do not "fix" `follow_camera.cpp`'s `normalize`.** It multiplies by a reciprocal
where `Vector3.Normalize` reads as a division; making it a division REGRESSES the sky
from 3.023% to 5.730%. (The rolling `orthonormal` is the opposite case: there the
oracle divides, and it is pinned bit-for-bit.)

**The 3DS sampler fix stays deferred with cause** — the 3DS pads index textures to POT
with transparent padding, so a plain `C3D_TexSetWrap(REPEAT)` would tile at the padded
period and sample discarded padding.

## Standing open items

Full list with evidence in `docs/OPEN_ITEMS.md`; the frame-record rule is in
`docs/frames/README.md` and the "the pin cannot show this" rule in
`docs/evidence/README.md`. The ones that bite: **the live frontend's un-interpolated
object transform** (review-5's remaining MAJOR, next slice); **C3D depth and blend
state still differ from the C#** (test disabled vs LESS_OR_EQUAL test/write;
`Pass::Blended` glass is opaque), which is why `renderer_c3d.cpp` ignores
`depth_layer` — and non-`Solid` batches are in **10 of level 3's 17** arenas; the
sprite residency bound is compile-verified only (the 3DS frontend binds no Kurt);
`depth_pull.cpp` is `DepthPull.Of` — Vulkan applies the pull, the C3D backend does
not; the oracle capture environment is not simplifiable (glslang flags,
`NUGET_PACKAGES`, the `unshare` namespace for `Xvfb :99`).

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
