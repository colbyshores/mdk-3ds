# Phase 2 continuation checkpoint

Risk: medium — review-11 remediation: the refusal gate's "arms adjudicated" counter now moves at the
verdict print instead of on the line before it, so a `PASS` can no longer be printed over zero verdicts,
and the archive fixture helper now refuses a name that does not fit the 12-byte field instead of
silently building a fixture whose bytes contradict its assertions. No shipped source, game behaviour,
renderer, golden, data format, `ralph-pi.sh` or `review_state.sh` change; the pinned frame is
byte-unchanged. Medium rather than low because it changes what an existing gate means — a tree that used
to go green through a silent early return now goes red — and the deliverable is a falsification record.

Last updated: 2026-11-01 (**batch 25** — review-11 remediation; the review had **no Critical and no
High** findings and its verdict was COMPLETE, and both Low findings were confirmed and fixed, none
rebutted; full narrative, falsification tables and the corrected mechanism in `docs/PROGRESS_LOG.md`
"Iteration 53"). **F1 Low:** `ADJUDICATED` sat *before* the verdict `if/else`, so it counted "reached
the line"; a `return 0` inserted after it printed `PASS SpritesRefusal (2 arms adjudicated)` over zero
verdict lines with exit 0 (reproduced on `af2fac3`). A `verdict` function is now the only place a
verdict prints and the only place the count moves, with `ARM_VERDICT` keeping it per-arm, so a
double-FAIL arm counts once and cannot over-report. Four placements tested; the header claim is now
what the mechanism actually pins. **F2 Low:** confirmed with the mechanism corrected — `memcpy` precedes
`putU32`, so a 13-char name does not corrupt the offset, the offset write truncates the **name** and
the assertions then blame `bni.cpp` for a bug in the test file. `makeArchive` now refuses it by name;
`testOverlongFixtureNameIsRefused` pins it (bound deleted → 2 checks FAILED, exit 1). Goldens
**442/0/0**, no PASS line moved; frame byte-unchanged. Previous: **batch 24** ("Iteration 52"),
**batch 23** ("Iteration 51"), **batch 22** ("Iteration 50").

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
   **NEXT, in priority order:** (a) **DONE — batch 22.** The missing-`TRAVSPRT.BNI` path now
   has a committed evidence pair (`docs/evidence/2026-11-01-L7-sprites-missing-scene.{png,txt}`,
   symlink farm + 4-byte archive, `DISPLAY=:99`, lavapipe, command and revision in the sidecar,
   which says in its own text that a pin cannot show an error path) and a gated pin for the guard
   behind it (`tests/scripts/archive_guard_tests.cpp`). (b) **F3 from review-1** — `dumpRunner` has no
   mixer field, so `mixer_.play("RUNNER", Once)` is unpinned; add the column to the probe *and* the
   harness and regenerate `L7_t120_runner_{a,b}` from the oracle. The probe is the scratch tree at
   `/tmp/mdk-pi/obj3` (a copy of `src/Mdk.Game` plus `Probe`, writable by this account) and already
   builds a real `SoundMixer` over `SoundBank.ForLevel`, so the value is obtainable. (c) The C3D depth
   and blend state still differ from the C# and the Citro3D backend ignores `MeshDraw::world`, so the
   3DS cannot place an object until it has a per-draw matrix. (d) The middle 173 px of the pinned frame
   is **not** the object pass — at frame 1 neither tree has a script object; the sky/middle/bottom
   split of the 1.240% gap, with the "do NOT re-litigate the panorama, palette, uniforms, projection or
   camera" note, is in `docs/OPEN_ITEMS.md`. (e) `FrameBlend.Lerp` is `Vector3.Lerp`
   (`fmaf(from, 1-t, to*t)`), 1 ULP from the port's spelling on one object at L3 t120, and `fmaf` is a
   libm call on devkitARM at `-mfpu=vfp`: a target codegen decision, not a substitution. Probe in
   `docs/OPEN_ITEMS.md`.

Deferred with cause: 248 `push_kurt` awaits knock-back; Rides writes the alarm countdown;
consumers of 135/195/202/203/81 remain partial; the 3DS sampler fix and Kurt's muzzle
flash, ride pose and footsteps; `Vector3.Lerp`'s rounding (above); the other half of
`FrameBlend` (Kurt's blended feet/yaw, `Record/Update/Turn/KurtAt`) and `TransformOf`'s
driven-carrier shift, both with `Rides`; review-1's F3 (the `RUNNER` voice). All with
evidence in `OPEN_ITEMS.md`. **Old-3DS timing is UNMEASURED.**

**Owner-authorized tasks (cannot be done in a batch).** (1) Add `ralph-pi.sh`, `ralph.sh`,
`docs/KILO_RALPH_LOOP_POLICY.md` and `.kilo/agent/*` to the `PROTECTED` set of
`tools/runner/gates_batch_integrity.py` — that file is in its own `PROTECTED` list, so only the owner
may widen it (review-1 F1). (2) Wire `bash tools/runner/test_review_state.sh` into `tools/check.sh` as
a gate line; `check.sh` is owner-protected, so the state machine is checked only by hand (batch 12).
(3) A review is recorded `done` on rc 0 + a non-empty report, so an `INCOMPLETE` verdict still clears
the owed range; whether it should is a policy change to `review_outcome` — read the report's `VERDICT:`
line (review-1 oq3, AR-1). (4) Whether the `invoked`-with-nothing-reconstructible last-resort halt
should halt at all, rather than assume the whole history is unreviewed (review-3 oq1, narrowed by
batch 14). (5) The shipped unit is `RestartPreventExitStatus=2 6 7`, so exit 8 (BLOCKED) **is**
restarted while 9 is not; and under batch 16's ordering two restarts in one review window end the loop
for good (review-3 F4, batch 17). (6) `ralph.sh:230` still carries the original **unchecked**
`state_set` and `8cc37ad` reverts `ralph.sh` deliberately ("Pi loop only"), so "one writer, one
refusal" is a property of `ralph-pi.sh` only; whether the Kilo loop is retired is the owner's call
(review-4 oq1). (7) The startup fold classifies an in-flight review as lost **without reading the run
dir**, where `review-$ITER.rc` and `review-$ITER.md` already say whether the reviewer finished — so a
stop after a completed review costs a strike and a second paid call. Batch 19 removed the second site
that could reach it; batch 20 found none left in the window. Changing the fold changes the meaning of
`REVIEW_FAILS` and the owed set (review-5 oq1, re-asked by review-6 oq1). (8) Whether the `Risk:` line
is the builder's to write at all: the builder authors the input to its own review policy. The safety net
overrode batch 20's `medium` (567-line diff) and batch 24's (505 lines) so the policy held, but the
coupling is real (review-7 oq3, re-asked by review-11 oq3). (9) Whether published evidence sidecars are
correctable in place: `docs/evidence/README.md` says *never edit a published pair*, which is why
review-9 F3's misquoted `git rev-list` range became an erratum in "Iteration 51" instead of an edit.
(10) Whether `tick1`'s data-missing `SKIP` should exit non-zero, i.e. whether "the golden suite could
not run" is a gate failure on hosts without the licensed tree. It changes what `make -C tests/scripts`
means for every operator and every CI box, so it is not a batch's call. (11) AGENTS.md §11 P7 names
`/tmp/kilo` as the capture directory, but it is `drwxr-xr-x museagent museagent` and **not writable by
this account** (`piagent`, uid 994) — both this batch and review-11 hit that. Either P7 names a
directory the builder can write, or the service user changes; until then every agent must pick its own
scratch path and say so in the evidence.

## Current tree state (VERIFIED)

The full record — every invariant, every failure mode, every "do not change this" — is the
appendix to `docs/PROGRESS_LOG.md`, "Current tree state (VERIFIED) — as of batch 13", marked
**superseded in part** where batches 14-25 changed it. The headline:
- **Camera/renderer:** the C#'s RIGHT-HANDED `CreateLookAt` basis; Vulkan folds clip.y into the
  matrix and the sky inverse carries the flip on its LEFT side; shaders use `M * v`; Citro3D culls
  per draw with the mode cached in the **Citro3D context**. **UNVERIFIED on hardware** (P6).
- **Frame golden:** level 3, 320x240, seed 1, frame 1; `oracle=b60e72552c40…`, `cpp=58615fc85d02…`
  (a regression baseline, NOT a fidelity reference), gap `1.240 / max 247 / mean 0.1819` at
  tolerance 0.
- **Kurt/sprites:** `game/kurt_sprite.{h,cpp}` is `Kurt/KurtSprite.cs`; palette-index frames
  (5.30 MiB measured) uploaded on first draw under `MdkCapabilities::sprite_residency_bytes` (0
  desktop, 1 MiB 3DS) — UNVERIFIED on hardware (P6). `Player::fall` is `Kurt.Fall` whole.
  **When `TRAVSPRT.BNI` cannot be parsed the frontend runs the level WITHOUT scripts and says so, and
  `tick1` exits 1 rather than hand the runtime a null archive** — the C# `Bni.Load` throws, so a null
  archive is a different game, not a degraded one. Do not reintroduce a `nullptr` into the
  `ScriptRuntime` constructor from a shipped entry point, and do not "tighten" `Bni::parse` into a
  content check: a zero-filled archive is a legal EMPTY archive and the C# can produce a missing entry,
  so `itemsAnimation` returning `nullptr` for a missing entry is faithful. The parse guard is pinned by
  `tests/scripts/archive_guard_tests.cpp` (offset and size, not just `has()`, since batch 23; the
  **name-field width** since batch 24 via `testFullNameFillsTheField`, whose 12-char no-NUL name is the
  only fixture where reading the field as 8 cannot pass; and the **fixture helper's own bound** since
  batch 25 via `testOverlongFixtureNameIsRefused`), the **refusal branch itself** by
  `tests/scripts/sprites_refusal_test.sh` — a 4-byte stub must exit non-zero with the message and an
  8-byte legal empty archive must exit 0 without it, same argv, no ROM data — and the scene is pictured
  in `docs/evidence/2026-11-01-L7-sprites-missing-scene.png`. That gate covers **`tick1`'s copy
  only**; the shipped frontend's copy at `mdk_linux/src/main.cpp:383-394` is ungated (Standing open
  items, review-10 F2).
- **Object pass:** `transform()` is `CreateScale * (rolling ? RollingBasis : Rotation) *
  CreateTranslation(...)` via `mdk::multiply`; `transform_at(alpha)` is what a frame draws with;
  `rotation()` keeps the libm trig and the `@vfp-exempt R6` pin; `roll` has a writer (`motionBank` is
  `ObjectMotion.Bank`); the Runner is `ObjectBehaviors.UpdateRunner`, and **`sprites` is a REQUIRED
  constructor argument** (since batch 11 no shipped entry point can pass `nullptr`).
  `mdk_render_draw_mesh` **queues** and the backend buckets by `Pass` in queue order — the oracle's
  order, not a backend's (lifetime in `render/draw_queue.h`, P3). `update_rolling()` runs **before**
  `previous_position = position`, where the C# does, and `mdk::create_from_axis_angle` is
  **bit-exact against .NET 10**.
- **Loop harness:** fail-closed on every way a mandatory review can fail to happen, and fail-*forward*
  where the obligation is recoverable: a lost review is owed, counted, and lands on `failed` — the halt
  is reserved for a state with no range, nothing owed and no `HEAD` naming an **ancestor** commit. Every
  transition goes through `state_write` (one temp file, one rename, **a checked status**: a record that
  cannot be written stops the loop at exit 9), and `state_set` is the single-key spelling of that SAME
  writer — **one writer, one refusal, one status**. **The obligation is created before the thing it must
  survive** — dispatch persists the in-flight range and `invoked` before the reviewer is spawned. **And
  nothing may sit between a verdict and the transition that records it — loop-wide**: the review counters
  land *after* it and the **gates phase performs no record write at all**. Independence is the resolved
  upstream and family, re-resolved after a fallback; the live cache is per-uid/per-proxy at 0600, checked
  on the descriptor it reads, never through a symlink, never blocked by a planted FIFO. State machine:
  `tools/runner/review_state.sh`, **177 cases**; how that suite lifts `ralph-pi.sh` (one `lift_fn`, one
  `guard_fragment`, three region lifts with behavioural cases instead, `count_writes` counting
  **occurrences** not lines) is in "Iteration 49" and **must not be re-loosened** (batches 20-21).

## Verified on this tree (batch 25)

All gates re-run from a clean invocation; verbatim output and five falsification tables in
`docs/PROGRESS_LOG.md`, "Iteration 53" — read those tables before calling any case there inert.
Goldens **exit 0** (`All cases match the oracle.`, **442 PASS / 0 FAIL / 0 SKIP**, no PASS line moved);
`tools/check.sh` **exit 0** (`gates_frames PASS L3_f1.bmp sha=58615fc85d02e009`, byte-identical to the
pinned `cpp=`, so `reference.sha256` was NOT regenerated; `gates_smdh` SKIPPED; integrity PASS);
`make -C mdk_3ds` **exit 0** (`3DSX mdk.3dsx`); `cmake --build build-linux --target mdk` **exit 0**;
`test_review_state.sh` **exit 0**, **177 cases**. The reversions that went red: a `return 0` where the
verdict counter used to sit (exit **1** where `af2fac3` printed `PASS` and exited **0**), a `return` in
one arm only (`1 of 2`), deleting the fixture bound (2 checks, `vm_tests` **exit 1**), and a
13-character width-pin fixture (the NOTE names the fixture before any assertion fails).
`archive_guard_tests.cpp` was restored after each reversion and re-verified green — **no shipped source
changed in this batch**. Frame `docs/frames/2026-11-01-verdict-counted-at-print.png` + `.txt` —
`1.240 / 247 / 0.1819`, byte-identical to its 30 predecessors. **No emulator run and no Old-3DS
hardware run: Old-3DS timing stays UNMEASURED**, Citro3D picture UNVERIFIED (P6). No gate was pinned,
relaxed or edited.

## Standing open items

Full list with evidence in `docs/OPEN_ITEMS.md`; the frame-record rule is in
`docs/frames/README.md` and the "the pin cannot show this" rule in `docs/evidence/README.md`. The
ones that bite: **C3D depth and blend state still differ from the C#** (test disabled vs
LESS_OR_EQUAL test/write; `Pass::Blended` glass is opaque), which is why `renderer_c3d.cpp` ignores
`depth_layer` — and non-`Solid` batches are in **10 of level 3's 17** arenas; the Citro3D backend
ignores `MeshDraw::world`, so the 3DS cannot place an object; the sprite residency bound is
compile-verified only (the 3DS frontend binds no Kurt); `depth_pull.cpp` is `DepthPull.Of` — Vulkan
applies the pull, the C3D backend does not; a late-frame oracle picture needs owner-authorized tooling
(`capture_oracle.sh` is pinned to frame 1). **Do not "fix" `follow_camera.cpp`'s `normalize`** (a
reciprocal where `Vector3.Normalize` reads as a division; making it a division REGRESSES the sky from
3.023% to 5.730%), and the 3DS sampler fix stays deferred with cause (the 3DS pads index textures to
POT with transparent padding, so a plain `C3D_TexSetWrap(REPEAT)` would tile at the padded period).
Answered rather than left implicit: a sidecar's "why this picture did not move / what shows the change
instead" narrative **is** the convention (`docs/frames/README.md`); `publish_frame.sh` emits only the
three measured lines and the narrative is appended by hand (batch 19 stopped, batch 20 resumed, batch 25
does it again). Class bounds worth keeping, each a *rule for the next batch* rather than a defect:
**a host harness must return the counter it accumulated on every exit path** (review-9 F1; every other
`return 0` in both harnesses was read and cleared in "Iteration 51"); **a multi-arm gate must state how
many arms it adjudicated and compare that against the declared number, and an arm that cannot set up is
a failure, not a skip** (review-10 F1; `sprites_refusal_test.sh` does this, and the other two shell
gates were read and already fail closed on a missing input, by a named mechanism); and from review-11
F1, the sibling of that rule — **a counter must move at the site of the thing it counts, not on the way
to it**, because a count taken before the verdict asserts comparisons that were never made and is
reachable through the very mechanism written to close the last defect; the first batch that adds a
multi-arm shell gate must take the count where the verdict is printed, and must not express an arm's
setup failure as a bare `return`. From review-11 F2, the fixture-side bound: **a test helper that
builds bytes must bound its own inputs against the field width it claims to pin** — and its refusal must
be a fixture diagnostic, not a check result, because a passing test that increments the failure counter
turns a green suite red (measured: `1 check(s) FAILED`, 52 PASS instead of 53). From review-7 oq1, a
latent harness gap with a named trigger: the collector death loop asserts the outcome **after** a kill,
not that the kill happened — `count_writes` counts text while `_die` counts executions, so a record
write moved onto an untaken branch would be counted by the derived bound, never reached by the kill, and
the case would assert the uninterrupted outcome and PASS. Sound as shipped (all four arm writes execute
on the `done` path). **The first batch that adds a conditional record write to the collector arm must
land the exit-status assertion (require `9` per iteration) with it.** From review-7 oq2, a bound on a
claim rather than a defect: `lift_fn` is bounded by the definition's own shape *while every definition
in `ralph-pi.sh` keeps its `}` in column 0*. And one declared gap rather than a fixed one: **the shipped
frontend's copy of the missing-archive refusal, `mdk_linux/src/main.cpp:383-394`, is gated by nothing**
(review-10 F2). `sprites_refusal_test.sh` gates `tests/scripts/tick1.cpp:1101-1117`, the harness's
copy, and its header says so. Narrowing `if (scriptAssetsReady && spritesReady)` there builds a runtime
against an unparsed `Bni` — the random-stream divergence — and every gate stays green. A behavioural gate
was rejected on reachability, not on effort: that branch sits under `data.found()` → `LevelData::load` →
`if (arena)` → `spawnPlayer()`, so it needs the licensed tree, and a gate that skips without the tree is
review-10 F1's defect reintroduced. Trigger that closes it: owner task (10) landing. And one sibling
left standing on purpose: **`tick1`'s own `SKIP: no MDK data` still exits 0**, so the golden suite can
skip into a green `make -C tests/scripts` on a host without `./MDK/`; it is visible as `SKIP` and as a
PASS count that drops from 442 to **3** (measured in "Iteration 52"), and making it fail is owner task
(10), not a batch.

## Verification commands

```
make -C tests/scripts clean && make -C tests/scripts   # goldens (includes sprites_refusal_test.sh)
sh tests/scripts/sprites_refusal_test.sh               # the missing-archive refusal, both arms
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
