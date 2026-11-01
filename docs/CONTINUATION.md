# Phase 2 continuation checkpoint

Risk: high — moves WHEN the loop is allowed to stop: the gates phase no longer writes the record,
so the last stop boundary inside the invoked-review window is gone and `LAST_GATES` now reaches
disk with the end-of-iteration rewrite instead of mid-iteration. Error handling, record
persistence and a trust boundary; the test suite moved (157 -> 163 cases) and all three lifted
fragments are now measured rather than trusted. No C/C++, Makefile, golden, frame-oracle tool or
game behaviour moved.

Last updated: 2026-11-01 (batch 19: run-20261101-062333/**review-5.md** dispositioned —
**R5-1/R5-2/R5-3 Medium, R5-4/R5-5/R5-6 Low: all six confirmed, none rebutted; no Critical or
High**). Batch 18 fixed one lifted fragment and one window; review-5 showed both were *classes
with a neighbour*, each reproduced on a scratch copy before it was changed. `STATE_WRITE_FN`
carried the same unbounded, prose-matching lift review-4 F2 found in `STATE_SET_FN`, one function
up: a one-line `state_write` over-ran 73 lines and reported `PASS AR6-4: … stops the loop` with
zero `exit 9` left in code — now bounded, comment-stripped, measured three ways. And the gates
arm wrote `LAST_GATES` while the record still said `invoked` with both in-flight markers set,
after a reviewer that may already have finished: a full disk there produced batch 18's harm from
a second site (a COMPLETED review folded as a lost one — owed, `REVIEW_FAILS + 1`, re-billed,
twice reaching the non-restartable exit 7). The gates phase now performs **no record write**; the
verdict lands with the end-of-iteration rewrite, and the shipped region is run against an
unwritable record to prove it. Four of the fourteen collector death cases could not inject their
failure; the bound is now derived and asserted. Iteration 46's Experiment B and review-4 open
question 2 are corrected in place. **163 cases** (153 carried, 10 added, 4 removed); reversions
each shown to FAIL; goldens **440/0/0**, no golden moved; the frame is byte-unchanged. Full
narrative: `docs/PROGRESS_LOG.md`, "Iteration 47".)

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
owner-protected. (3) A decision only the owner can make, from review-1's open question 3 and
confirmed by AR-1 (High): a review is recorded `done` on rc 0 + a non-empty report, so an
`INCOMPLETE` verdict still clears the owed range — including commits a widened range swept in.
Whether it should is a policy change to `review_outcome`, not a builder's call; the proposal,
if accepted, is to read the report's `VERDICT:` line and treat anything but a pass as `failed`.
Batch 15 did not make that change — it removed the code's claim that widening costs "at most
one redundant review", which was only true under the policy this task is about. (4) Narrowed
by batch 14, from
review-3's open question 1: the `invoked`-with-nothing-reconstructible halt is now the
last-resort branch only. Whether even *that* should halt rather than assume the whole history
is unreviewed is an owner policy call; the builder kept the halt, because a range nobody can
name is a range nobody can prove covered. (5) New from batch 17, from review-3's F4: the
shipped unit is `RestartPreventExitStatus=2 6 7`, so exit 8 (BLOCKED) **is** restarted while
exit 9 is not. The code comment now describes that as shipped and argues 8 restarting is
intended (a blocked route can recover when a worker route comes back), but changing the unit is
the owner's call, not a builder's. Also open from review-3's open questions: under batch 16's
ordering every kill inside one review window costs one of `REVIEW_FAIL_LIMIT=2` strikes and
exit 7 is non-restartable, so two restarts in one window end the loop for good. (6) New from batch 18, from review-4's open
question 1: `ralph.sh:230` still carries the original **unchecked** `state_set` (`mv` status
discarded). Commit `8cc37ad` reverts `ralph.sh` deliberately ("Pi loop only"), so the builder
treated it as intended dead code and left it alone — but then the "one writer, one refusal"
property is a property of `ralph-pi.sh` only. Whether the Kilo loop is retired or needs the same
fix is the owner's call. (7) New from batch 19, from review-5's open question 1: the startup fold
classifies an in-flight review as lost **without looking at the run dir**, where `review-$ITER.rc`
and `review-$ITER.md` already say whether the reviewer finished. That is why a stop after a
completed review costs a strike and a second paid call instead of being recognised as a finished
review whose verdict was never applied. Making the fold consult the run dir is a change to the
meaning of `REVIEW_FAILS` and the owed set, so it is the owner's call, not a builder's; batch 19
removed the second site that could reach it and left the policy alone.

## Current tree state (VERIFIED)

The full record — every invariant, every failure mode, every "do not change this" — is the
appendix to `docs/PROGRESS_LOG.md`, "Current tree state (VERIFIED) — as of batch 13", marked
**superseded in part** where batches 14-19 changed it. The headline:
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
  owed and no `HEAD` naming an **ancestor** commit. Every transition of the state file goes
  through `state_write` (one temp file, one rename, **and a checked status**: a record that
  cannot be written stops the loop at exit 9 rather than continuing from a stale one), and
  `state_set` is the single-key spelling of that SAME writer — **one writer, one refusal, one
  status**. **The obligation is created before the thing it must survive** — dispatch persists
  the in-flight range and `invoked` before the reviewer is spawned. **And nothing may sit
  between a verdict and the transition that records it — loop-wide, not just in the collector**:
  the review counters land *after* that transition and the **gates phase performs no record
  write at all**, because a stop boundary before the transition leaves `invoked` with a counter
  or a verdict half-written, which is a completed review the fold charges as a lost one;
  `LAST_GATES` reaches disk with the end-of-iteration rewrite. The only writes left inside the
  invoked window are the transition and the counters behind it; the blocked and skipped arms are
  alternatives that never leave `invoked` set. The suite asserts each transition's **key set**,
  not a substring of one call; a pin is scoped to the function or window that owns the property;
  **all three** scoping lifts are measured (one definition, no comment line, `bash -n`
  completeness); and every write counter matches a write anywhere in the line, not only at its
  start.  Independence is the resolved upstream and family; the identity is re-resolved after a
  fallback; the live cache is per-uid/per-proxy at 0600, checked on the descriptor it reads,
  never through a symlink, never blocked by a FIFO planted at the path (`O_NONBLOCK` makes the
  open return, `S_ISREG` makes it refuse), and the gate provably runs **before** the payload is
  consumed; the state machine is `tools/runner/review_state.sh`, asserted by **163 cases** in
  `tools/runner/test_review_state.sh`.

## Verified on this tree (batch 19)

All gates re-run from a clean invocation; verbatim output in `docs/PROGRESS_LOG.md`,
"Iteration 47". Goldens **exit 0** — `All cases match the oracle.`, **440 PASS / 0 FAIL /
0 SKIP**, no golden moved. `bash tools/check.sh` — **exit 0**, `CHECK: ALL GATES PASSED`
(`gates_frames PASS L3_f1.bmp sha=58615fc85d02e009`, byte-identical to the pinned `cpp=`, so
`reference.sha256` was NOT regenerated; `gates_smdh` SKIPPED, no CIA; integrity PASS).
`make -C mdk_3ds` — **exit 0**, `3DSX mdk.3dsx`. `cmake --build build-linux --target mdk` —
**exit 0**. `test_review_state.sh` — **exit 0**, **163 cases**, reversions each shown to FAIL.
`model_identity.py selftest --live` — `SELFTEST CLEAN`, 28 claims. Frame
`docs/frames/2026-11-01-gates-arm-window.png` + `.txt` — `1.240 / 247 / 0.1819`,
byte-identical to its 24 predecessors. **No emulator run and no Old-3DS hardware run: Old-3DS
timing stays UNMEASURED**, Citro3D picture UNVERIFIED (P6). No gate was pinned, relaxed or
edited. The full record of every invariant is the `docs/PROGRESS_LOG.md` appendix, "Current tree
state (VERIFIED)", superseded in part by batches 14-19 as marked.

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
