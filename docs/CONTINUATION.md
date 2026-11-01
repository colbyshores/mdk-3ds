# Phase 2 continuation checkpoint

Risk: high — rewrites the loop's review-record control plane: how reviewer independence is
decided (resolved upstream/family, not the alias string), what a dispatched-then-died review
persists (`failed`, not `done`), and what survives a restart (the owed commit range). Control
flow, persisted state and error handling; no game arithmetic, render path or golden moved.

Last updated: 2026-11-01 (batch 12: review-2's five findings all dispositioned — **F1/F2/F3
Medium fixed**, **F5 Low fixed**, **F4 Medium deferred** with cause and now priority item (a).
In short: a dispatched-then-died reviewer can no longer be recorded as reviewed
(`review_rc_normalise`/`review_outcome`; the old `[ "$RRC" -ne 0 ]` returned 2 on a
non-numeric operand, which `if` read as FALSE); the owed commit range and `REVIEW_FAILS`
survive a restart and outrank the risk policy; independence is the resolved upstream **and
family**, not the alias (the live proxy confirms the two DeepSeek aliases are one family on two
hosts); and the `vm_tests` `nullptr` claim no longer rests on a property the fixture lacks.
Goldens **440 PASS / 0 FAIL / 0 SKIP**, no golden moved; the pinned frame is byte-unchanged.
Narrative: `docs/PROGRESS_LOG.md`, "Iteration 40". Keep this file under ~150 lines.)

## PRIORITY ORDER (owner directive 2026-10-25)

1. **DONE — mixer call-sites** (`ObjectMotion.cs:133`, `ObjectBehaviors.cs:90-115`).
2. **DONE — `Debris.Shatter` + opcodes 137/138/139.** BreakUp/Trail/Updraft await Effects.
3. **DONE for the C# oracle — opcode conversion.** 211 dispatches pinned in
   `docs/OPCODE_COVERAGE.md`; the remaining 27 each need a deferred subsystem. Do not
   blind-port them or read them off `MDKD3D.EXE`.
4. **DONE — P5.0 frame-golden harness** (`gates_frames.py` is gate 7; proven to fail).
5. **DONE — review-30's 3DS texture-residency MAJOR** (decode at load, upload on first draw
   under a named 1 MiB budget, restore on eviction).
6. **DONE — the object pass, all the way to the GPU**, plus review-3/-5/-6's findings on it
   (batch 8: `ObjectMotion.Bank`; batches 9-10: the Runner, both halves pinned).
   **NEXT, in priority order:** (a) **F4 from review-2** — the missing-`TRAVSPRT.BNI` path
   has no committed evidence pair. Batch 11 changed a §11 P6 missing-asset path and shipped
   the 17th byte-identical pin; the only capture of the new behaviour was `/tmp/f2demo/`,
   which is not in the repository. Ship
   `docs/evidence/2026-11-01-L7-sprites-missing-scene.{png,txt}` (symlink farm + a 4-byte
   archive so the marker exists, the read succeeds and the parse fails; `DISPLAY=:99`,
   lavapipe, command and revision in the sidecar) and say in the pinned sidecar that a pin
   cannot show an error path. (b) **F3 from review-1** — `dumpRunner` has no mixer field,
   so `mixer_.play("RUNNER", Once)` is unpinned; add the column to the probe *and* the
   harness and regenerate `L7_t120_runner_{a,b}` from the oracle. (c) The C3D depth and
   blend state still differ from the C# (Standing open items) and the Citro3D backend
   ignores `MeshDraw::world`, so the 3DS cannot place an object until it has a per-draw
   matrix. (d) The middle 173 px of the pinned frame, which is **not** the object pass — at
   frame 1 neither tree has a script object. The sky/middle/bottom split of the 1.240% gap,
   with the "do NOT re-litigate the panorama, palette, uniforms, projection or camera" note,
   is in `docs/OPEN_ITEMS.md`. (e) `FrameBlend.Lerp` is `Vector3.Lerp` and the port's
   `from + (to-from)*t` is not its rounding — the oracle's is `fmaf(from, 1-t, to*t)`,
   costing 1 ULP on one object at L3 t120. `fmaf` is a libm call on devkitARM at
   `-mfpu=vfp` and the current spelling contracts to one `vmla.f32` per component
   **whatever `-ffp-contract` says**: a target codegen decision, not a substitution. Probe
   in `docs/OPEN_ITEMS.md`.

Deferred with cause: 248 `push_kurt` awaits knock-back; Rides writes the alarm countdown;
consumers of 135/195/202/203/81 remain partial; the 3DS sampler fix and Kurt's muzzle
flash, ride pose and footsteps; `Vector3.Lerp`'s rounding (above); the other half of
`FrameBlend` (Kurt's blended feet/yaw, `Record/Update/Turn/KurtAt`) and `TransformOf`'s
driven-carrier shift, both with `Rides`; review-1's F3 (the `RUNNER` voice). All with
evidence in `OPEN_ITEMS.md`. **Old-3DS timing is UNMEASURED.**

**Owner-authorized tasks (cannot be done in a batch).** (1) From review-1's F1 remediation:
add `ralph-pi.sh`, `ralph.sh`, `docs/KILO_RALPH_LOOP_POLICY.md` and `.kilo/agent/*` to the
`PROTECTED` set of `tools/runner/gates_batch_integrity.py` — that file is gate tooling and is
in its own `PROTECTED` list, so only the owner may widen it. (2) From this batch: wire
`bash tools/runner/test_review_state.sh` into `tools/check.sh` as a gate line. It is not
wired, so the review-record state machine is currently checked only by hand; `tools/check.sh`
is owner-protected and adding a gate line is exactly the kind of change a builder must not
make to obtain a green.

## Current tree state (VERIFIED)

**Camera and renderer.** `buildLookAtCamera` uses the C#'s RIGHT-HANDED `CreateLookAt`
basis and perspective projection. Vulkan folds clip.y into the combined matrix; the sky
inverse carries the flip on its LEFT side; shaders use `M * v`. Citro3D uses
`GPU_CULL_BACK_CCW` with the mirror removed and culls per draw (Solid culls back, the other
passes do not), the mode cached in the **Citro3D context**. **The 3DS result is UNVERIFIED
on hardware** (P6).

**Frame golden.** `tests/frames/reference.sha256` pins level 3, 320x240, seed 1, frame 1:
`oracle=b60e72552c40…`, `cpp=58615fc85d02…` (a regression baseline, NOT a fidelity
reference), gap `1.240 / max 247 / mean 0.1819` at tolerance 0: determinism,
byte-exactness to `cpp=`, and a ceiling that may only shrink.

**Kurt, sprites, death.** `game/kurt_sprite.{h,cpp}` is `Kurt/KurtSprite.cs` (world quad at
the feet, one sprite pixel = 1/360 of the view height, hotspot 101 px up, two row bands at
the oracle's floor crossings). The port keeps the sprite frames' palette indices (5.30 MiB
for `TRAVSPRT.BNI`, measured) and uploads on first draw under
`MdkCapabilities::sprite_residency_bytes` (0 on the desktop, 1 MiB on the 3DS) — UNVERIFIED
on hardware (P6). `Player::fall` is `Kurt.Fall` whole.

**Object pass (place, pose, layout, roll, bank, queue, GPU, blend, behaviours).**
`MdkObject::transform()` is `CreateScale * (rolling ? RollingBasis : Rotation) *
CreateTranslation(pos + z*lift)`, in that order, via `mdk::multiply` (System.Numerics
element arithmetic: one rounded product + three `fma`). `pose_parts()` is `PoseParts`, from
caches keyed by model address, invalidated by `scripts::forgetPoseCaches()` and
`MdkObject::forget_poses()`. `game/object_view.{h,cpp}` is `ObjectView`, keyed by
`(model, palette)`; the stream refuses a batch that would pass `kObjectStreamCapacity`
(`1 << 18`, the C#'s `Renderer.StreamVertices`). The GPU half is `game/object_draw.{h,cpp}`
(ONE dynamic mesh, uploaded once per frame, one draw per queued batch at its offset).
**The transform a frame draws with** is `transform_at(alpha)` (`remember()` snapshots
place/angles/arena at the top of each tick; `blends()` refuses to interpolate across
`alpha >= 1`, a missing snapshot, an arena change or a jump — `frame_blend::jumped` is
`MaxSpeed * seconds` squared, so a teleport is drawn where it ends, never on the way), and
`rotation()` is `rotation_of(roll, pitch, yaw)` through the same libm trig and the same
`@vfp-exempt R6` pin. **`roll` has a writer** (batch 8): `motionBank` is `ObjectMotion.Bank`,
per object per tick between `update_rolling` and `previous_position = position`, where the C#
calls `Bank`. **The built-in behaviours have the Runner**: `motionRunner` is
`ObjectBehaviors.UpdateRunner`, called from `motionPickup` where the C# calls it — after the
chute has gone (`attached == nullptr`), before the Still-list spin — and its `H150_R` /
`H150_I` come from `itemsAnimation`, the C#'s `Items.GetAnimation`, parsed out of the
sprites archive the runtime borrows. The idle chance is `rng().next(32768) < 72`, drawn
**only** when the object is idle or its animation is done: that is the oracle's stream, not
an approximation of it. **`sprites` is a REQUIRED
constructor argument** (as in the C#): without the archive there is no `H150_R`/`H150_I`
and, because the idle draw is guarded on the animation, the random stream differs. Since
batch 11 **no shipped entry point can pass `nullptr`**: `mdk_linux` builds the runtime only
under `scriptAssetsReady && spritesReady`, and `tick1` exits 1 when the archive does not
parse (a zero-filled archive *does* parse as empty, and the L7 goldens then fail 31 cases —
that is the pin). `vm_tests` still passes `nullptr` out loud, and it is inert there — but not
for the reason the record used to give ("the level has no `SW_H150`" is **false**:
`vm_tests.cpp:3593` loads real level 7, which has one). The true reason is that `sprites_`
reaches the RNG by exactly one route, `itemsAnimation` → `motionRunner`, and `motionRunner`
runs only for `type_name == "SW_H150"`, which no `vm_tests` case creates. `itemsAnimation`
still answers `nullptr` for a missing **entry**, because `Items.cs:150` does: do not "guard"
that. Both halves of the Runner are pinned: the idle by `L7_t1000_*`, the run by
`L7_t120_runner_{a,b}`.

**Draw submission: order and lifetime.** `mdk_render_draw_mesh` **queues**; the backend
submits the frame's draws bucketed by `Pass` — Solid, DoubleSided, Mirror, Blended, OnTop,
Overlay, queue order inside a bucket — `Renderer.cs:1000-1004, 1049-1057`, shared as
`render/draw_queue.{h,cpp}` because the order is the oracle's, not a backend's; the flush
point is the end of the frame's 3D scene. Lifetime (AGENTS.md P3) is in `render/draw_queue.h`:
a Vulkan dynamic mesh writes the slot its own frame draws from and a released mesh is
destroyed two frames out; the 3DS defers `linearFree` to the next `C3D_FrameBegin`.

**Rolling (`ObjectMotion.Roll`).** `MdkObject::update_rolling()` turns the basis about the
horizontal axis across the tick's movement, and `runTick` calls it **before**
`previous_position = position`, where the C# does. `mdk::create_from_axis_angle` is
**bit-exact against .NET 10** because .NET builds that matrix from a quaternion; the
look-alike element-wise spelling differs by 2-8 ULP per element.

**Loop harness (`ralph-pi.sh`).** A mandatory review is fail-closed on **both** halves of "it
did not happen". Not dispatchable ⇒ BLOCKED, `REVIEW_FAILS++`, exit 8. Dispatched and died ⇒
`REVIEW_STATUS=failed` and the commit stays **owed**: `REVIEW_OWED_BASE` and `REVIEW_FAILS` are
persisted, re-read at startup, and outrank the risk policy, so a restart cannot forget an
unreviewed commit and the next dispatch covers `owed..HEAD`. Independence is the **resolved
upstream and model family**, never the alias (`tools/runner/model_identity.py`: live
`/model/info`, else committed map, else NOTHING — and NOTHING is refused, because "could not
tell" must not reach the same path as "proved different"); `review-$ITER.identity.txt` records
both sides. The state machine is `tools/runner/review_state.sh`, asserted by
`tools/runner/test_review_state.sh` (44 cases, with decay guards proven to FAIL if the old
spellings come back). The builder prompt injects the owner's generic loop policy under a
project-precedence block, not as "authoritative". **Testing this found one more F1-shaped
bug**: the startup read `case "${REVIEW_FAILS:-0}" in …) REVIEW_FAILS=0` tests the
*defaulted* value and assigns to the original, so a state file predating the key left
`REVIEW_FAILS` **empty** — and `[ "" -ge 2 ]` errors with status 2, which `&&` reads as FALSE,
so the reviewer-failure limit silently never fired. `review_int` replaces it, and the old
spelling is pinned as a failing case in its own right.

## Verified on this tree (batch 12)

All three gates were re-run from a clean invocation; numbers are quoted from their own output,
and the narrative is `docs/PROGRESS_LOG.md`, "Iteration 40".

- `make -C tests/scripts clean && make -C tests/scripts -j8` — **exit 0**, ended
  `All cases match the oracle.`, **440 PASS / 0 FAIL / 0 SKIP**, no existing golden moved.
- `bash tools/check.sh` — **exit 0**, `CHECK: ALL GATES PASSED` (`audit_c_with_classes` 0
  violations, `gates_vfp` blockers=0, `audit_arm11_math` clean, `audit_r11_idiv` no
  candidates, `gates_core_parity` clean, `gates_smdh` SKIPPED (no CIA), `gates_frames PASS`
  with `L3_f1.bmp` byte-identical to the pinned `cpp=`, so `reference.sha256` was NOT
  regenerated, `gates_batch_integrity PASS`). No gate was pinned, relaxed or edited.
- `make -C mdk_3ds -j8` — **exit 0**, built `mdk.3dsx`; `cmake --build build-linux --target
  mdk -j8` — **exit 0**. The change is shell and Python, so neither target was at risk; both
  were built anyway rather than assumed.
- `bash tools/runner/test_review_state.sh` — **exit 0**, 44/44, and proven to FAIL when a
  fixed spelling is reintroduced.
- Frame `docs/frames/2026-11-01-review2-review-record.png` + `.txt`: `1.240 / 247 / 0.1819`,
  byte-identical to its predecessors, as expected when nothing in the render path or the
  simulation moved. **No emulator run and no Old-3DS hardware run: Old-3DS timing stays
  UNMEASURED** and the Citro3D picture UNVERIFIED (P6).

## Standing open items

Full list with evidence in `docs/OPEN_ITEMS.md`; the frame-record rule is in
`docs/frames/README.md` and the "the pin cannot show this" rule in
`docs/evidence/README.md`. The ones that bite: **C3D depth and blend state still differ
from the C#** (test disabled vs LESS_OR_EQUAL test/write; `Pass::Blended` glass is opaque),
which is why `renderer_c3d.cpp` ignores `depth_layer` — and non-`Solid` batches are in
**10 of level 3's 17** arenas; the Citro3D backend ignores `MeshDraw::world`, so the 3DS
cannot place an object; the sprite residency bound is compile-verified only (the 3DS
frontend binds no Kurt); `depth_pull.cpp` is `DepthPull.Of` — Vulkan applies the pull, the
C3D backend does not; the oracle capture environment is not simplifiable (glslang flags,
`NUGET_PACKAGES`, the `unshare` namespace for `Xvfb :99`), and `capture_oracle.sh` is pinned
to frame 1, so a late-frame oracle picture (level 7, say) needs owner-authorized tooling.

**Do not "fix" `follow_camera.cpp`'s `normalize`** (it multiplies by a reciprocal where
`Vector3.Normalize` reads as a division; making it a division REGRESSES the sky from 3.023%
to 5.730%), and the 3DS sampler fix stays deferred with cause (the 3DS pads index textures
to POT with transparent padding, so a plain `C3D_TexSetWrap(REPEAT)` would tile at the
padded period). Both with evidence in `docs/OPEN_ITEMS.md`.

## Verification commands

```
make -C tests/scripts clean && make -C tests/scripts   # goldens
tests/scripts/build/tick1 dump draw 3 1                # the queued draws (also `bound`)
tests/scripts/build/tick1 dump blend 3 1               # the blended transforms (also `blendcases`)
tests/scripts/build/tick1 dump transform 7 1000        # the hamster's animation
tests/scripts/build/tick1 dump runner 7 120 -21,-13,0.73   # the hamster's run, per tick
bash tools/check.sh                                    # all gates
bash tools/runner/test_review_state.sh                 # the review-record state machine
python3 tools/runner/model_identity.py selftest --live # alias→upstream map vs the live proxy
DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds
cmake --build build-linux --target mdk -j8             # the Linux frontend
python3 tools/runner/gates_frames.py                   # gate 7 alone
tests/frames/regenerate_reference.sh                   # intentional frame change only
```

Tests do not track header dependencies: run `make -C tests/scripts clean` after
header changes. Never weaken a gate or test.
