# Phase 2 continuation checkpoint

Risk: high — batch 30 moved the Citro3D frame's draws: the per-eye pass (target, matrix, state) left
`begin_frame`, where it ran before a single draw existed and every draw therefore landed in the LAST
installed target — the RIGHT one, while the panel shows the LEFT one in mono, i.e. a top screen of nothing
but the clear colour — and now runs in `end_frame`, one eye at a time, immediately before that eye's
replay of the queue. `pica::eyePlan` decides how many passes there are (one with the 3D slider closed, two
with it open) and owns the slider clamp and each eye's sign. Target-specific GPU state on a backend whose
picture cannot be looked at here — no 3DS emulator, no Old-3DS, so P6 stays open. Mitigations, measured
not asserted: `C3D_FrameDrawOn` and `C3D_SetFrameBuf` were disassembled out of the linked `libcitro3dd.a`
rather than reasoned about, the mono skip is Forsaken's measured behaviour on this hardware ("slider = 0
disables stereo and restores full framerate"), the plan is host-tested and **7 mutations of it each turn
the suite red** (table in `docs/PROGRESS_LOG.md` "Iteration 58"), and both targets build. High rather
than medium because this is the frame's draw order on an unverifiable target — and because the bug it
fixes is a black screen, which makes the missing hardware the most expensive gap in the port.

Last updated: 2026-11-02 (**batch 30** — the per-eye replay; full narrative, the `libcitro3dd.a`
disassembly, the seven-mutation table and the gate output are in `docs/PROGRESS_LOG.md` "Iteration 58").
Batch 29's "the left eye gets only its clear" was worse than written: `begin_frame` installed both eyes
before any draw was queued, so `flushDraws` at `end_frame` put the whole frame into the RIGHT target with
the right eye's matrix, and the panel shows the LEFT one with the slider closed — a top screen of clear
colour. `installEye` + `replayQueue` now run once per eye from `end_frame`, `pica::eyePlan` (clamped,
signed, host-tested) decides one pass or two, and both buffers are still cleared every frame. Goldens
**443/0/0** (the new case is inside `PASS PicaMatrix`), no PASS line moved; frame byte-unchanged, and its
sidecar says why. **This is the first 3DS-renderer batch whose expected effect on a real console is
visible**, which is what makes P6's missing hardware the port's most expensive open gap. The tree still
carries the owner's uncommitted harness work (`AGENTS.md`, `ralph/builder.md`, `ralph-pi.sh`,
`gates_batch_integrity.py`, an untracked frame pair) — three of them are in the integrity gate's own
`PROTECTED` set, so batches keep staging only their own paths. Previous: **batch 29** ("Iteration 57", the
Citro3D depth state), **batch 28** ("Iteration 56", the per-draw world matrix), **batch 27** ("Iteration
55", review-1 F3), **batch 26** ("Iteration 54", review-12).

**Batch 27, in full**: `ObjectBehaviors.cs:305-306`
enters `H150_R` **and** plays `RUNNER`; the port does both at `script_runtime.cpp:1033-1034` but the
runner goldens printed no mixer field, so a dropped, renamed or mis-gated `play()` passed every gate in
the repository. The column is on both sides of the seam — `tests/scripts/tick1.cpp:255-266` and the oracle
probe's `runner` mode — and the column varies (`mixer=-` t1-t3, `mixer=RUNNER` from t4), so it is exercised
rather than constant. Prior-solution lookup: **`pi-knowledge` is UNAVAILABLE, not empty** — `knowledge_show`
returns `No knowledge bases.`, so there is no `mdk-sdl` KB and an empty search is an environment gap, not a
finding (the loop's `ralph/builder.md` now requires that distinction; that edit and a `ralph-pi.sh` edit
both appeared mid-iteration, are not this batch's, and are left uncommitted for the owner — the owner has
since landed them, in `4aa49ea`); the Obsidian vault was searched read-only for `SW_H150`, `SoundMixer`, `RUNNER voice`, `mixer_.play`
and `golden provenance` with **zero files**; Cognee MCP is not exposed in this harness. Goldens
**442/0/0**, no PASS line moved; frame byte-unchanged. Previous: **batch 26** ("Iteration 54", review-12:
all four findings — 2 Medium, 2 Low — confirmed and fixed, none rebutted, the verdict counter moved after
the print it counts, and the three owner-authorized clauses batch 25 dropped restored), **batch 25**
("Iteration 53"), **batch 24** ("Iteration 52"), **batch 23** ("Iteration 51").

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
   behind it (`tests/scripts/archive_guard_tests.cpp`). (b) **DONE — batch 27.** `dumpRunner` prints a
   `mixer=` column, so `mixer_.play("RUNNER", Once)` is pinned, and `L7_t120_runner_{a,b}` are regenerated
   from the oracle. The probe is the scratch tree at `/tmp/mdk-pi/obj3` (a copy of `src/Mdk.Game` plus
   `Probe`, writable by this account) and builds a real `SoundMixer` over `SoundBank.ForLevel`; rebuild it
   with `/tmp/mdk-pi/build_probe.sh`, then `DOTNET_ROOT=/tmp/kilo/dotnet10 MDK_DATA_DIR=…/MDK ./mdkprobe 7
   120 1 runner -21,-13,0.73` (and `-26,-18,0.73`) re-derives both files. Its `src/**.cs` differs from the
   read-only oracle only in the three known print-only instrumented files. (c) **PARTLY DONE — batches 28
   and 29.** The Citro3D backend composes each queued draw's `MeshDraw::world` against the frame's eye
   matrix (`mdk_3ds/src/render/pica_matrix.{h,cpp}`, pinned by `PASS PicaMatrix`), so an object can be
   placed where its script put it, and the frame now **tests depth** with the oracle's `LESS_OR_EQUAL`
   and the oracle's per-pass write rule (`pica::depthStateForPass`, clear `0xFFFFFF`). What is LEFT of
   (c) is the **blend** half only: no `C3D_AlphaBlend`, so `Pass::Blended` glass is opaque — and it is
   blocked on a named cause, because the palette expansion forces alpha 255 and the vertex shader
   writes `outclr = ones`, so there is no alpha to blend with yet (`OPEN_ITEMS.md`).
   (d) The middle 173 px of the pinned frame
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
driven-carrier shift, both with `Rides`. All with
evidence in `OPEN_ITEMS.md`. **Old-3DS timing is UNMEASURED.**

**Owner-authorized tasks (cannot be done in a batch).** (1) Add `ralph-pi.sh`, `ralph.sh`,
`docs/KILO_RALPH_LOOP_POLICY.md` and `.kilo/agent/*` to the `PROTECTED` set of
`tools/runner/gates_batch_integrity.py` — that file is in its own `PROTECTED` list, so only the owner
may widen it (review-1 F1). (2) Wire `bash tools/runner/test_review_state.sh` into `tools/check.sh` as
a gate line; `check.sh` is owner-protected, so the state machine is checked only by hand (batch 12).
(3) A review is recorded `done` on rc 0 + a non-empty report, so an `INCOMPLETE` verdict still clears
the owed range; whether it should is a policy change to `review_outcome` — the proposal is to read the
report's `VERDICT:` line (review-1 oq3, AR-1). (4) Whether the `invoked`-with-nothing-reconstructible last-resort halt
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
coupling is real and an owner check on it belongs on record (review-7 oq3, re-asked by review-11 oq3,
re-asked again by review-12 F2). (9) Whether published evidence sidecars are
correctable in place: `docs/evidence/README.md` says *never edit a published pair*, which is why
review-9 F3's misquoted `git rev-list` range became an erratum in "Iteration 51" instead of an edit.
If in-place correction of a **prose** claim is acceptable while the pixels and measurements stay
frozen, that is a one-line change to that README. (10) Whether `tick1`'s data-missing `SKIP` should exit non-zero, i.e. whether "the golden suite could
not run" is a gate failure on hosts without the licensed tree. It changes what `make -C tests/scripts`
means for every operator and every CI box, so it is not a batch's call. (11) AGENTS.md §11 P7 names
`/tmp/kilo` as the capture directory, but it is `drwxr-xr-x museagent museagent` and **not writable by
this account** (`piagent`, uid 994) — both this batch and review-11 hit that. Either P7 names a
directory the builder can write, or the service user changes; until then every agent must pick its own
scratch path and say so in the evidence. (12) **CLOSED by batch 29** — the `mdk-sdl`
**pi-knowledge base now exists** (`knowledge_show`: `mdk-sdl`, 3881 chunks / 936 files, plus
`tetrisphere-godot`), so the mandatory prior-solution lookup can actually run; batch 29's is the first
that did, and it is what surfaced the sibling-project depth probes this batch checked its answer against.
Batches should still report an *empty search* as a search that found nothing, not as proof of absence, and
still record a source's unavailability when one is genuinely absent (Cognee MCP is not exposed in the Pi
harness).

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
  batch 25 via `testOverlongFixtureNameIsRefused`, and a refusal that no case accounted for fails the
  suite since batch 26), the **refusal branch itself** by
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

## Verified on this tree (batch 30)

All gates re-run from a clean invocation; verbatim output, the citro3d disassembly and the falsification
are in `docs/PROGRESS_LOG.md`, "Iteration 58". Goldens **exit 0** (`All cases match the oracle.`, **443
PASS / 0 FAIL / 0 SKIP**, no PASS line moved — the new eye-plan case is inside the existing `PASS
PicaMatrix`); `tools/check.sh` **exit 0** (`CHECK: ALL GATES PASSED`; `gates_frames PASS
L3_f1.bmp sha=58615fc85d02e009`, byte-identical to the pinned `cpp=`, so `reference.sha256` was NOT
regenerated; `gates_smdh` SKIPPED; integrity PASS); `make -C mdk_3ds` **exit 0** (`3DSX mdk.3dsx`, no
warnings); `cmake --build build-linux --target mdk` **exit 0**. Two things were measured rather than
asserted. **The mechanism**: `.text.C3D_FrameDrawOn` in the linked `libcitro3dd.a` is
`strb r3,[r0,#24]; add r0,r0,#8; bl C3D_SetFrameBuf; ldrh r3,[r4,#18]; ldrh r2,[r4,#16]; mov r1,#0;
mov r0,#0; bl C3D_SetViewport` and nothing else, and `C3D_SetFrameBuf` copies the descriptor into the
context at `+0x1d8` and sets dirty bit 32 — so the target is selected for the draws that *follow*, and an
install in `begin_frame` was overwritten before a draw existed. **Load-bearingness**: seven mutations of
`pica::eyePlan` — pass count both ways, the offsets swapped, the upper clamp removed, the half-separation
dropped, the closed-slider test, the lower clamp moved — each turn the suite red from `failures=0`. What
the suite cannot reach is stated: the replay loop itself and the `reset()` after the last eye are covered
by reading and by both builds, not by a test. Frame
`docs/frames/2026-11-02-c3d-per-eye-replay.png` + `.txt` — `1.240 / 247 / 0.1819`, byte-identical to its
predecessors, because the capture is the Linux backend. **No emulator run and no Old-3DS hardware run:
Old-3DS timing stays UNMEASURED and the Citro3D picture stays UNVERIFIED (P6)** — and this time the
expected change on a console is visible, so the gap is the port's most expensive. No gate was pinned,
relaxed or edited. Batch 29's own record — the nine depth mutations and the `libcitro3dd.a` reading of
`C3D_DepthMap`/`C3D_FrameBufClear` — is in "Iteration 57".

## Standing open items

Full list with evidence in `docs/OPEN_ITEMS.md`; the frame-record rule is in
`docs/frames/README.md` and the "the pin cannot show this" rule in `docs/evidence/README.md`. The
ones that bite: **C3D blend state still differs from the C#** (no `C3D_AlphaBlend`, so
`Pass::Blended` glass is opaque) and it is blocked on a named cause — the palette expansion forces alpha
255 and `mdk_c3d.v.pica` writes `outclr = ones`, so there is no alpha to blend with yet; the **depth**
half of that item landed in batch 29 (`LESS_OR_EQUAL`, the per-pass write rule, clear `0xFFFFFF`, pinned
by `PASS PicaMatrix`), and non-`Solid` batches are in **10 of level 3's 17** arenas; the per-eye replay
**landed in batch 30** (`installEye` + `replayQueue` per eye in `end_frame`, `pica::eyePlan` for the
count/sign/clamp) — and it turned out to have been a black screen, not a stereo gap: the single pass that
did happen went to the right target while the panel shows the left one in mono. Two follow-ups it left:
the canvas/inset flush points now run *before* the per-eye replay (neither is called on the 3DS today,
and `canvas_quad` is a no-op there), and `gfxSet3D` is called once at init rather than per frame from the
slider, which is the common homebrew pattern but not what Forsaken does); the sprite residency bound is
compile-verified only (the 3DS frontend binds no Kurt); `depth_pull.cpp` is `DepthPull.Of` — Vulkan
applies the pull and the C3D backend still ignores `depth_layer`, now for a different reason: the depth
test exists, but the pull needs the **view position and the primitive**, which the render seam does not
hand the C3D backend (a seam question, not a substitution); a late-frame oracle picture needs owner-authorized tooling
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
F1, refined by **review-12 F1**, the sibling of that rule — **a counter must move AFTER the observable it
counts, never before it**, because a backstop that fires on a SHORT count cannot see an over-count, so a
count taken ahead of the print turns a skipped print into a false PASS while a count taken after it turns
the same skip into a red gate. Batch 25 moved the count to the print site but left it ahead of the
`printf`, which re-opened the defect it closed (measured again on `07345d6`); the direction, not just the
location, is the rule. The first batch that adds a multi-arm shell gate must take the count after the
verdict prints, and must not express an arm's setup failure as a bare `return`. From review-11 F2, the
fixture-side bound, extended by **review-12 F4**: **a test helper that builds bytes must bound its own
inputs against the field width it claims to pin**, its refusal must be a fixture diagnostic rather than a
check result — because a passing test that increments the failure counter turns a green suite red
(measured: `1 check(s) FAILED`, 52 PASS instead of 53) — **and that diagnostic must be accounted for**,
because a refusal that only prints a NOTE is invisible to a caller that asserts the negative and passes
vacuously on the empty bytes. Count the refusals, count the asserted refusals, and require them to be
equal; a flag set by one case and cleared by another is order-dependent. From review-7 oq1, a
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
