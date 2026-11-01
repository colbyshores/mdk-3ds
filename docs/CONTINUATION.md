# Phase 2 continuation checkpoint

Risk: high — changes how the loop decides whether a batch was reviewed: an `invoked` review
 with no in-flight range now reconstructs or keeps its obligation instead of halting, the
 un-collected fold lands on `failed` rather than manufacturing the state its own refusal
 rejects, dispatch persists the range before the status derived from it, and the identity
 cache checks ownership on the descriptor it reads (`O_NOFOLLOW` + `fstat`) instead of a
 stat-then-read pair that followed symlinks. Persisted state, error handling and a trust
 boundary; no game arithmetic, render path, golden, test or interface moved.

Last updated: 2026-11-01 (batch 14: review-3's six findings all dispositioned — **F1 High,
F2 High, F3 Medium, F4/F5/F6 Low: all confirmed, all fixed, none rebutted** — plus one
self-found weak pin). In short: an `invoked` with no in-flight range no longer halts when an
owed base is already on disk, and reconstructs the obligation from the state file's `HEAD`
when nothing is (that base is the previous iteration's commit, hence an ancestor of the
unreviewed one, so the range over-covers rather than under-covers); the un-collected fold now
writes `REVIEW_STATUS=failed` before clearing the range, so it cannot manufacture the state
its own refusal rejects; dispatch persists the range before the status derived from it; the
cache's `st_uid` refusal is finally tested and both cache paths refuse a symlink instead of
reading or truncating through it; and the F2 decay guards are scoped to the fold body, because
a whole-file grep for a line that also appears in another arm is a pin that does not pin.
`test_review_state.sh` is **86 cases**, every new guard shown to FAIL when its fix is
reverted (six reversion experiments). Goldens **440 PASS / 0 FAIL / 0 SKIP**, no golden moved;
the pinned frame is byte-unchanged and its sidecar says so. Narrative: `docs/PROGRESS_LOG.md`,
"Iteration 42". Keep this file under ~150 lines.)

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
   has no committed evidence pair (batch 11's only capture was `/tmp/f2demo/`, not in the
   repo). Ship `docs/evidence/2026-11-01-L7-sprites-missing-scene.{png,txt}` (symlink farm + a
   4-byte archive so the marker exists, the read succeeds and the parse fails; `DISPLAY=:99`,
   lavapipe, command and revision in the sidecar) and say in the pinned sidecar that a pin
   cannot show an error path. (b) **F3 from review-1** — `dumpRunner` has no mixer field, so
   `mixer_.play("RUNNER", Once)` is unpinned; add the column to the probe *and* the harness and
   regenerate `L7_t120_runner_{a,b}` from the oracle. (c) The C3D depth and blend state still
   differ from the C# and the Citro3D backend ignores `MeshDraw::world`, so the 3DS cannot
   place an object until it has a per-draw matrix. (d) The middle 173 px of the pinned frame is
   **not** the object pass — at frame 1 neither tree has a script object; the sky/middle/bottom
   split of the 1.240% gap, with the "do NOT re-litigate the panorama, palette, uniforms,
   projection or camera" note, is in `docs/OPEN_ITEMS.md`. (e) `FrameBlend.Lerp` is
   `Vector3.Lerp` and the port's `from + (to-from)*t` is not its rounding — the oracle's is
   `fmaf(from, 1-t, to*t)`, 1 ULP on one object at L3 t120; `fmaf` is a libm call on devkitARM
   at `-mfpu=vfp` and the current spelling contracts to one `vmla.f32` per component **whatever
   `-ffp-contract` says**: a target codegen decision, not a substitution. Probe in
   `docs/OPEN_ITEMS.md`.

Deferred with cause: 248 `push_kurt` awaits knock-back; Rides writes the alarm countdown;
consumers of 135/195/202/203/81 remain partial; the 3DS sampler fix and Kurt's muzzle
flash, ride pose and footsteps; `Vector3.Lerp`'s rounding (above); the other half of
`FrameBlend` (Kurt's blended feet/yaw, `Record/Update/Turn/KurtAt`) and `TransformOf`'s
driven-carrier shift, both with `Rides`; review-1's F3 (the `RUNNER` voice). All with
evidence in `OPEN_ITEMS.md`. **Old-3DS timing is UNMEASURED.**

**Owner-authorized tasks (cannot be done in a batch).** (1) From review-1's F1 remediation:
add `ralph-pi.sh`, `ralph.sh`, `docs/KILO_RALPH_LOOP_POLICY.md` and `.kilo/agent/*` to the
`PROTECTED` set of `tools/runner/gates_batch_integrity.py` — that file is gate tooling and is in
its own `PROTECTED` list, so only the owner may widen it. Review-1's open question 2 makes the
same point from the other side: consecutive batches have edited the script that decides whether
the builder's work is reviewed, and it is audited only by the model it dispatches. (2) From
batch 12: wire `bash tools/runner/test_review_state.sh` into `tools/check.sh` as a gate line;
it is not wired, so the review-record state machine is checked only by hand, and `check.sh` is
owner-protected. (3) A decision only the owner can make, from review-1's open question 3: a
review is recorded `done` on rc 0 + a non-empty report, so an `INCOMPLETE` verdict still clears
the owed range — including carried-forward commits a widened range swept in. Whether it should
is a policy change to `review_outcome`, not a builder's call. (4) Narrowed by batch 14, from
review-3's open question 1: the `invoked`-with-nothing-reconstructible halt is now the
last-resort branch only. Whether even *that* should halt rather than assume the whole history
is unreviewed is an owner policy call; the builder kept the halt, because a range nobody can
name is a range nobody can prove covered.

## Current tree state (VERIFIED)

The full record — every invariant, every failure mode, every "do not change this" — is the
appendix to `docs/PROGRESS_LOG.md`, "Iteration 41". The headline:
- **Camera/renderer:** the C#'s RIGHT-HANDED `CreateLookAt` basis; Vulkan folds clip.y into the
  matrix and the sky inverse carries the flip on its LEFT side; shaders use `M * v`; Citro3D
  culls per draw with the mode cached in the **Citro3D context**. **UNVERIFIED on hardware** (P6).
- **Frame golden:** level 3, 320x240, seed 1, frame 1; `oracle=b60e72552c40…`,
  `cpp=58615fc85d02…` (a regression baseline, NOT a fidelity reference), gap
  `1.240 / max 247 / mean 0.1819` at tolerance 0.
- **Kurt/sprites:** `game/kurt_sprite.{h,cpp}` is `Kurt/KurtSprite.cs`; palette-index frames
  (5.30 MiB measured) uploaded on first draw under `MdkCapabilities::sprite_residency_bytes`
  (0 desktop, 1 MiB 3DS) — UNVERIFIED on hardware (P6). `Player::fall` is `Kurt.Fall` whole.
- **Object pass:** `transform()` is `CreateScale * (rolling ? RollingBasis : Rotation) *
  CreateTranslation(...)` via `mdk::multiply`; `transform_at(alpha)` is what a frame draws with;
  `rotation()` keeps the libm trig and the `@vfp-exempt R6` pin; `roll` has a writer
  (`motionBank` is `ObjectMotion.Bank`); the Runner is `ObjectBehaviors.UpdateRunner`, and
  **`sprites` is a REQUIRED constructor argument** (since batch 11 no shipped entry point can
  pass `nullptr`). `mdk_render_draw_mesh` **queues** and the backend buckets by `Pass` in queue
  order — the oracle's order, not a backend's; lifetime in `render/draw_queue.h` (P3).
  `update_rolling()` runs **before** `previous_position = position`, where the C# does, and
  `mdk::create_from_axis_angle` is **bit-exact against .NET 10**.
- **Loop harness:** fail-closed on every way a mandatory review can fail to happen, and
  fail-*forward* on the ones where the obligation is recoverable: a lost review is owed,
  counted, and lands on `failed` — the halt is reserved for a state with no range, nothing
  owed and no `HEAD` naming a commit. Independence is the resolved upstream and family; the
  identity is re-resolved after a worker fallback; the live cache is per-uid/per-proxy at
  0600, checked on the descriptor it reads and never through a symlink; the state machine is
  `tools/runner/review_state.sh`, asserted by 86 cases in `tools/runner/test_review_state.sh`.

## Verified on this tree (batch 14)

All three gates were re-run from a clean invocation; the full verbatim output is in
`docs/PROGRESS_LOG.md`, "Iteration 42".

- `make -C tests/scripts clean && make -C tests/scripts -j8` — **exit 0**, ended
  `All cases match the oracle.`, **440 PASS / 0 FAIL / 0 SKIP**, no existing golden moved.
- `bash tools/check.sh` — **exit 0**, `CHECK: ALL GATES PASSED`: `audit_c_with_classes` 0
  violations, `gates_vfp` 0 blockers, `audit_arm11_math` clean, `audit_r11_idiv` no
  candidates, `gates_core_parity` clean, `gates_smdh` SKIPPED (no CIA), `gates_frames PASS`
  (`L3_f1.bmp sha=58615fc85d02e009`, byte-identical to the pinned `cpp=`, so
  `reference.sha256` was NOT regenerated), `gates_batch_integrity PASS`. No gate was pinned,
  relaxed or edited.
- `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds -j8` — **exit
  0**, `3DSX mdk.3dsx`; `cmake --build build-linux --target mdk -j8` — **exit 0**, `Built
  target mdk`. The diff is shell and Python, so neither target was at risk; both were built
  anyway rather than assumed.
- `bash tools/runner/test_review_state.sh` — **exit 0**, `REVIEW STATE: ALL CASES PASS`, **86
  cases**, every new guard shown to FAIL when its fix is reverted (six reversion experiments,
  quoted in the log entry); `model_identity.py selftest --live` — **exit 0**, `SELFTEST CLEAN`
  (9 identity + 15 cache claims).
- Frame `docs/frames/2026-11-01-review3-owed-obligation.png` + `.txt`: `1.240 / 247 /
  0.1819`, byte-identical to its 19 predecessors (md5 `6769a137…`), and the sidecar says in
  its own text why the pin cannot show a review-record change (review-1 M2). **No emulator run
  and no Old-3DS hardware run: Old-3DS timing stays UNMEASURED**, Citro3D picture UNVERIFIED
  (P6).

## Standing open items

Full list with evidence in `docs/OPEN_ITEMS.md`; the frame-record rule is in
`docs/frames/README.md` and the "the pin cannot show this" rule in
`docs/evidence/README.md`. The ones that bite: **C3D depth and blend state still differ from
the C#** (test disabled vs LESS_OR_EQUAL test/write; `Pass::Blended` glass is opaque), which is
why `renderer_c3d.cpp` ignores `depth_layer` — and non-`Solid` batches are in **10 of level 3's
17** arenas; the Citro3D backend ignores `MeshDraw::world`, so the 3DS cannot place an object;
the sprite residency bound is compile-verified only (the 3DS frontend binds no Kurt);
`depth_pull.cpp` is `DepthPull.Of` — Vulkan applies the pull, the C3D backend does not; a
late-frame oracle picture needs owner-authorized tooling (`capture_oracle.sh` is pinned to frame
1). **Do not "fix" `follow_camera.cpp`'s `normalize`** (a reciprocal where
`Vector3.Normalize` reads as a division; making it a division REGRESSES the sky from 3.023% to
5.730%), and the 3DS sampler fix stays deferred with cause (the 3DS pads index textures to POT
with transparent padding, so a plain `C3D_TexSetWrap(REPEAT)` would tile at the padded period).

## Verification commands

```
make -C tests/scripts clean && make -C tests/scripts   # goldens
tests/scripts/build/tick1 dump draw 3 1                # the queued draws (also `bound`)
tests/scripts/build/tick1 dump blend 3 1               # the blended transforms (also `blendcases`)
tests/scripts/build/tick1 dump runner 7 120 -21,-13,0.73   # the hamster's run, per tick
bash tools/check.sh                                    # all gates
bash tools/runner/test_review_state.sh                 # the review-record state machine
python3 tools/runner/model_identity.py selftest --live # alias→upstream map vs the live proxy
DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds
cmake --build build-linux --target mdk -j8             # the Linux frontend
tests/frames/regenerate_reference.sh                   # intentional frame change only
```

Tests do not track header dependencies: run `make -C tests/scripts clean` after header changes.
Never weaken a gate or test.
