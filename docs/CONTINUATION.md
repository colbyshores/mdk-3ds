# Phase 2 continuation checkpoint

Risk: medium — changes the loop's own test harness (one shared bounded lift, one shared fragment
guard, a write counter that now counts calls) plus a documentation record correction. `ralph-pi.sh`,
`ralph.sh`, every gate, every golden, the frame-oracle tooling and all game behaviour are untouched;
root cause of all three findings is certain and each fix was reproduced before and after.

Last updated: 2026-11-01 (batch 20: run-20261101-062333/**review-6.md** dispositioned — **F1
Medium, F2 Low, F3 Low: all three confirmed, none rebutted; no Critical or High**). F1 was a defect
in the *record*: the R5-2 reversion block named a one-arm edit and quoted a two-arm measurement, so
a reviewer re-running it got `got '1'` where the record said `got '2'` — the "the named edit is not
the one whose output is quoted" class, recurring inside the block written to close it. Re-run on
three scratch worktrees (both arms / `then` alone / `else` alone) and corrected in place as three
named experiments, each quoting only its own output. F2 was the fourth lift of the shape batches
18-19 fixed — unmeasured, reading the raw file with the terminator R5-1 removed — so **one**
`lift_fn` now bounds every function lift by the definition's own shape and **one** `guard_fragment`
runs the same three measurements on each of the five fragments; the blanket "all three lifts are
measured" claim is replaced by what each class actually carries. F3: the counter that derives the
collector death-loop bound counted lines while the loop injects at write *calls* — now occurrences,
plus two self-tests. **175 cases** (157 labels carried, 6 re-worded with a successor each, 12 new
checks, 0 lost); six reversion experiments each shown to FAIL; goldens **440/0/0**, no golden moved;
the frame is byte-unchanged. Narrative: `docs/PROGRESS_LOG.md`, "Iteration 48".

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
   `Vector3.Lerp` (`fmaf(from, 1-t, to*t)`), 1 ULP from the port's spelling on one object at
   L3 t120, and `fmaf` is a libm call on devkitARM at `-mfpu=vfp`: a target codegen decision,
   not a substitution. Probe in `docs/OPEN_ITEMS.md`.

Deferred with cause: 248 `push_kurt` awaits knock-back; Rides writes the alarm countdown;
consumers of 135/195/202/203/81 remain partial; the 3DS sampler fix and Kurt's muzzle
flash, ride pose and footsteps; `Vector3.Lerp`'s rounding (above); the other half of
`FrameBlend` (Kurt's blended feet/yaw, `Record/Update/Turn/KurtAt`) and `TransformOf`'s
driven-carrier shift, both with `Rides`; review-1's F3 (the `RUNNER` voice). All with
evidence in `OPEN_ITEMS.md`. **Old-3DS timing is UNMEASURED.**

**Owner-authorized tasks (cannot be done in a batch).** (1) Add `ralph-pi.sh`, `ralph.sh`,
`docs/KILO_RALPH_LOOP_POLICY.md` and `.kilo/agent/*` to the `PROTECTED` set of
`tools/runner/gates_batch_integrity.py` — that file is in its own `PROTECTED` list, so only the
owner may widen it (review-1 F1). (2) Wire `bash tools/runner/test_review_state.sh` into
`tools/check.sh` as a gate line; `check.sh` is owner-protected, so the state machine is checked only
by hand (batch 12). (3) A review is recorded `done` on rc 0 + a non-empty report, so an `INCOMPLETE`
verdict still clears the owed range; whether it should is a policy change to `review_outcome` — the
proposal is to read the report's `VERDICT:` line (review-1 oq3, AR-1). (4) Whether the
`invoked`-with-nothing-reconstructible last-resort halt should halt at all, rather than assume the
whole history is unreviewed (review-3 oq1, narrowed by batch 14). (5) The shipped unit is
`RestartPreventExitStatus=2 6 7`, so exit 8 (BLOCKED) **is** restarted while 9 is not; and under
batch 16's ordering two restarts in one review window end the loop for good (review-3 F4, batch 17).
(6) `ralph.sh:230` still carries the original **unchecked** `state_set` and `8cc37ad` reverts
`ralph.sh` deliberately ("Pi loop only"), so "one writer, one refusal" is a property of
`ralph-pi.sh` only; whether the Kilo loop is retired is the owner's call (review-4 oq1). (7) The
startup fold classifies an in-flight review as lost **without reading the run dir**, where
`review-$ITER.rc` and `review-$ITER.md` already say whether the reviewer finished — so a stop after
a completed review costs a strike and a second paid call. Batch 19 removed the second site that
could reach it; batch 20 found none left in the window. Changing the fold changes the meaning of
`REVIEW_FAILS` and the owed set (review-5 oq1, re-asked by review-6 oq1).

## Current tree state (VERIFIED)

The full record — every invariant, every failure mode, every "do not change this" — is the
appendix to `docs/PROGRESS_LOG.md`, "Current tree state (VERIFIED) — as of batch 13", marked
**superseded in part** where batches 14-20 changed it. The headline:
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
  order — the oracle's order, not a backend's (lifetime in `render/draw_queue.h`, P3).
  `update_rolling()` runs **before** `previous_position = position`, where the C# does, and
  `mdk::create_from_axis_angle` is **bit-exact against .NET 10**.
- **Loop harness:** fail-closed on every way a mandatory review can fail to happen, and
  fail-*forward* where the obligation is recoverable: a lost review is owed, counted, and lands on
  `failed` — the halt is reserved for a state with no range, nothing owed and no `HEAD` naming an
  **ancestor** commit. Every transition goes through `state_write` (one temp file, one rename, **a
  checked status**: a record that cannot be written stops the loop at exit 9), and `state_set` is
  the single-key spelling of that SAME writer — **one writer, one refusal, one status**. **The
  obligation is created before the thing it must survive** — dispatch persists the in-flight range
  and `invoked` before the reviewer is spawned. **And nothing may sit between a verdict and the
  transition that records it — loop-wide**: the review counters land *after* it and the **gates
  phase performs no record write at all**, because a stop boundary before the transition leaves
  `invoked` with a half-written verdict; `LAST_GATES` reaches disk with the end-of-iteration
  rewrite. Independence is the resolved upstream and family, re-resolved after a fallback; the live
  cache is per-uid/per-proxy at 0600, checked on the descriptor it reads, never through a symlink,
  never blocked by a planted FIFO. State machine: `tools/runner/review_state.sh`, **175 cases**.
- **How that suite reads `ralph-pi.sh` (batch 20, do not re-loosen):** the script cannot be
  sourced, so pieces are lifted out and run. **Every function lift goes through one `lift_fn`**,
  bounded by the definition's own shape (a one-line definition ends on its own line; a multi-line
  one ends at the first `}` in column 0 — a bare `^\}` over-runs a one-liner and "the line ends in
  `}`" truncates a function containing `{ …; }` compounds), from the **comment-stripped** copy, and
  **every lifted fragment goes through one `guard_fragment`** (exactly one definition, no comment
  line, `bash -n` complete). The three **region** lifts (`STARTUP_BLOCK`, `COLLECT_WINDOW`/
  `COLLECT_ARM`, `GATES_REGION`) carry non-vacuity line counts plus behavioural cases, not the
  fragment guards — say it that way, do not claim "all lifts". `count_writes` counts
  **occurrences**, because the collector death loop injects at write *calls*; the bound it derives
  is asserted and the loop is `seq 1 $bound`, never a literal. Every reversion in
  `docs/PROGRESS_LOG.md` names the edit it applied and quotes only that edit's output.

## Verified on this tree (batch 20)

All gates re-run from a clean invocation; verbatim output in `docs/PROGRESS_LOG.md`,
"Iteration 48". Goldens **exit 0** — `All cases match the oracle.`, **440 PASS / 0 FAIL /
0 SKIP**, no golden moved. `bash tools/check.sh` — **exit 0**, `CHECK: ALL GATES PASSED`
(`gates_frames PASS L3_f1.bmp sha=58615fc85d02e009`, byte-identical to the pinned `cpp=`, so
`reference.sha256` was NOT regenerated; `gates_smdh` SKIPPED; integrity PASS).
`make -C mdk_3ds` — **exit 0**, `3DSX mdk.3dsx`. `cmake --build build-linux --target mdk` —
**exit 0**. `test_review_state.sh` — **exit 0**, **175 cases**, six reversions each shown to FAIL.
`model_identity.py selftest --live` — `SELFTEST CLEAN`, 28 claims. Frame
`docs/frames/2026-11-01-lifts-bounded-record-reproduces.png` + `.txt` — `1.240 / 247 / 0.1819`,
byte-identical to its 25 predecessors. **No emulator run and no Old-3DS hardware run: Old-3DS
timing stays UNMEASURED**, Citro3D picture UNVERIFIED (P6). No gate was pinned, relaxed or edited.

## Standing open items

Full list with evidence in `docs/OPEN_ITEMS.md`; the frame-record rule is in
`docs/frames/README.md` and the "the pin cannot show this" rule in `docs/evidence/README.md`. The
ones that bite: **C3D depth and blend state still differ from the C#** (test disabled vs
LESS_OR_EQUAL test/write; `Pass::Blended` glass is opaque), which is why `renderer_c3d.cpp` ignores
`depth_layer` — and non-`Solid` batches are in **10 of level 3's 17** arenas; the Citro3D backend
ignores `MeshDraw::world`, so the 3DS cannot place an object; the sprite residency bound is
compile-verified only (the 3DS frontend binds no Kurt); `depth_pull.cpp` is `DepthPull.Of` — Vulkan
applies the pull, the C3D backend does not; a late-frame oracle picture needs owner-authorized
tooling (`capture_oracle.sh` is pinned to frame 1). **Do not "fix" `follow_camera.cpp`'s
`normalize`** (a reciprocal where `Vector3.Normalize` reads as a division; making it a division
REGRESSES the sky from 3.023% to 5.730%), and the 3DS sampler fix stays deferred with cause (the 3DS
pads index textures to POT with transparent padding, so a plain `C3D_TexSetWrap(REPEAT)` would tile
at the padded period). From review-6 oq3, answered rather than left implicit: a sidecar's "why this
picture did not move / what shows the change instead" narrative **is** the convention
(`docs/frames/README.md`); `publish_frame.sh` emits only the three measured lines and the narrative
is appended by hand, which batch 19 stopped doing and batch 20 does again.

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
