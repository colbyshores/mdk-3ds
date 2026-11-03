# Phase 2 continuation checkpoint

Risk: medium — batch 54 changes shipped arithmetic on four per-object-per-tick sites in
`script_runtime.cpp` (friction factor, the projectile's box slab, the gun-aim cone floor, the
footing sweep's half height), each `fminf`/`fmaxf` replaced by the file's existing
NaN-propagating `mathMin`/`mathMax` so the port matches the oracle's `MathF.Min`/`MathF.Max`.
The difference is NaN-only — every finite input is bit-identical, and the friction pin asserts
the finite row in .NET's own bits — but it is behaviour, on four sites, in the motion path,
and two of the four cannot be pinned (with the unreachability measured, not assumed), so this
is not a low-risk localised diff. 478 cases pass, 3 of 5 mutants killed and 2 recorded
unkillable with the reason, the pinned frame is byte-identical (`sha 6646adf1dae3…`) because
frame 1 has no NaN, and the ARM object loses its `fmaxf`/`fminf` callouts. Review-24 is fully
dispositioned (F3/F4 fixed with measurement, F2 blocked by the frame-immutability gate, F1
owner-only); no owner file is staged.

Last updated: 2026-11-03 (**batch 54** — the last four `MathF.Min`/`MathF.Max` sites in
`script_runtime.cpp` stop being `fminf`/`fmaxf`: the friction factor (`ObjectMotion.cs:1066`),
`flyProjectile`'s `SegmentEntry` slab (`ScriptMath.cs:110-111`), `canSeeKurt`'s cone floor
(`ScriptRuntime.cs:3244`) and `sweepBox`'s half height (`ObjectMotion.cs:1138`). Four new
pins whose expected numbers are the C# probe's own printed output (new probe mode
`nanclamp`, transcript in the log), the cone's NaN unreachability measured rather than
argued, and review-24 F3's `MathF.Sign(NaN)` claim confirmed by running it — the oracle
throws, so `camera_roll.cpp`'s comment and the pin name now say "divergence". The record —
the probe transcript, the four pins, the 5-mutant table with the two unkillables and the
`nm -u` before/after — is `docs/PROGRESS_LOG.md`, "Iteration 82". Batch 53's record
(`CameraRoll`, the key-window schedule, the 9-mutant table, the `math_lut.h` negative-angle
measurement) is "Iteration 81". Batch 52's record (one `accelerate()`, the probe table and the 6-mutant
table) is "Iteration 80". Batch 51's
record (the fly-arrival pin and the three-mutant guard table, the F1 unclosability proof, the F3
rebuttal) is "Iteration 79". Batch 50's record (the six clamp pins,
the .NET derivation, the F4 rebuttal) is "Iteration 78". Batch 49's own record (the dispatch→arm pins, the clamp change,
the NaN reachability argument, its 8-mutant table) is "Iteration 77"; batch 48's — `chase`/
`followFormation`/`turnTowards`/`moveToward` and their derivation and 16-mutant tables — is "Iteration
76". Earlier batches are one "Iteration N" section back each: 54 = 82, 53 = 81, 52 = 80, 51 = 79, 50 = 78, 49 = 77, 48 = 76, 47 = 75,
46 = 74, 45 = 73, 44 = 72,
43 = 71, 42 = 70, 41 = 69, 40 = 68, 39 = 67, 38 = 66, 37 = 65, 36 = 64, 35 = 63, 34 = 62, 33 = 61,
32 = 60.
Review-18's F1-F6 were owner-only too, each reproduced before being dispositioned: in a clone of the
committed tree a `PROTECTED` edit fails the gate, editing a historical frame sidecar fails it ("history
is immutable"), and the committed `gates_batch_integrity.py` does not even accept `--each-commit` — that
whole recovery exists only in the unstaged owner diff, so there is nothing in the committed tree for a
batch to change. Review-19 F3 and review-20 F3 add one line each to that same owner task: `--each-commit`
with no `--base` prints `BATCH INTEGRITY: PASS` over zero commits (reproduced again in batches 50 and
51), while the **committed** gate rejects the flag as unrecognized (rc 2), so the fail-open path exists
only in the uncommitted owner diff. Review-21 F1 is that same task's `PROTECTED` half and review-21 F8
is its frame half (the untracked `ralph-self-heal` pair); review-21 F3 is **closed by the runtime**, not
by this batch: both KBs' stored `embedding_signature` now ends `prefixes=off`, matching the override.

**Still owner-gated and still uncommitted in the tree** (so batches stage only their own paths and
never `git add -A`): `AGENTS.md`, `ralph/builder.md`, `ralph-pi.sh`,
`tools/runner/gates_batch_integrity.py`, `docs/frames/2026-11-02-ralph-self-heal.{png,txt}`.
Review-10 F1/F2/F3/F6, review-12 B, review-13 R13-B, review-15 F1/F2, review-16 V16-1,
review-17 R34-1/R34-5 and review-18 F1-F6 are the same hazard under new numbers: the `--each-commit`
recovery that replaces the guard's fail-closed path, a green tree that depends on uncommitted files, the
un-invoked `--self-test`, `--each-commit` dropping the one-frame-per-**batch** rule, `--staged` accepting
what the range check rejects, and a historical sidecar whose counts are wrong and that the immutability
rule will not let a batch correct. All owner tasks (1)/(9)/(13).

## PRIORITY ORDER (owner directive 2026-10-25)

1. **DONE — mixer call-sites** (`ObjectMotion.cs:133`, `ObjectBehaviors.cs:90-115`).
2. **DONE — `Debris.Shatter` + opcodes 137/138/139.** BreakUp/Trail/Updraft await Effects.
3. **DONE for the C# oracle — opcode conversion.** 211 dispatches pinned in
   `docs/OPCODE_COVERAGE.md`; the remaining ones each need a deferred subsystem. Do not
   blind-port them or read them off `MDKD3D.EXE`.
4. **DONE — P5.0 frame-golden harness** (`gates_frames.py` is gate 7; proven to fail).
5. **DONE — review-30's 3DS texture-residency MAJOR** (decode at load, upload on first draw
   under a named 1 MiB budget, restore on eviction).
6a. **DONE — the combat half of `Kurt` is live state, not write-only fields (batches 40-46).**
    `Player` owns the combat half and the **health** lives in it alone (`ScriptRuntime::kurt()`
    returns `player_->combat()`; a runtime with no Player falls back to `unboundKurt_`) — pin
    `golden/L3_kurt_hurt.txt`. The **state** lives in `Kurt` alone (`Player::_state` is gone),
    which is what lets `hurt()`'s `Dead|Knocked|GetUp|HardLand` guard fire — pin
    `golden/L3_kurt_hurtland.txt`. The **recent-damage counter** declines and a hard landing
    zeroes it — pin `golden/L3_kurt_knock.txt`. The **invulnerability timer** declines ahead of
    every early return `Player::update` has — pin `golden/L3_kurt_invuln.txt`. The **knock-down**
    itself landed in batch 44: `ArenaSpace::floor` (the C#'s `Floor`, arenas around both ends,
    nearest crossing — NOT `floorCrossing`), `Player::knockDown`, `Player::updateKnockDamage`
    whole (three guards, the 13-unit air probe with its slam to -64, the decline as its tail),
    the `Knocked`→`GetUp`→`Still` arms (K_BANG 25 frames, push cleared at the flip, K_BFLIP 27),
    and the walk-input suppression — pins `golden/L3_kurt_knockdown.txt`,
    `golden/L3_floor_probes.txt`. The **combat fades** landed in batch 45: `Kurt::fadeFlashes`
    (both fields fall by `kFlashFade` = 4/tick, floored at zero) called where `Kurt.cs:159` puts
    it — after the Dead return, so a corpse fades nothing and a flash that was up when he died
    stays up; the death transition's `HurtFlash = 0`; `fadeSkullIn` + `skullFinished` so
    `Player::updateDeath` freezes the corpse past `SkullEnd` and reports the step it passes it
    (the C#'s `Died` event as the `Player::died()` latch §10 allows); and `Player::damage`
    promoted to the C#'s `Damage` whole — pin `golden/L3_kurt_flashes.txt` (621 lines, eight
    mutants). **Batch 46 landed opcode 248 `push_kurt`** and **batch 47 re-pinned it so the pin is
    real**: `ScriptRuntime::pushKurt` takes the whole operand list; mode 0 uses the C#'s
    asymmetric object-frame formula (`x = -a*c - b*s`, `y = -b*c - a*s`) with the lift at
    `args[3]`, other modes normalize `KurtPosition - object.Position` (the runtime's snapshot,
    not live feet) with the +X zero-distance fallback and the lift at `args[2]`;
    `Player::knockDown(push)` receives it before the vertical lift; the guard refuses an
    already-Knocked Kurt and a corpse. `golden/L3_kurt_push.txt` (361 lines, sha256 `39d21a6b…`)
    is byte-identical to the C# probe with **every mode-0 operand non-zero**, and eleven mutants
    were built and killed (nine by the golden, two — live-feet and reciprocal-normalize — only by
    the exact-float VM assertions, which is why those assertions exist).
    **NEXT, same family, in order:** `StopFiring` waits for the firing slice; `Revive` (the C#'s
    other writer of `HurtFlash = 0`, `Kurt.Combat.cs:196`) waits for the death-screen/save flow
    the `Died` event leads to (`Game.cs:448` — no menu, no save here); the nuke/explosion/teleport
    writers of `whiteFlash_` (`ScriptRuntime.cs:479,1734`, `Items.cs:365,527`, `EndLevel.cs:146`)
    wait for their own subsystems, so only opcode 215 can raise that field today; and
    `Knocked`/`GetUp` have no sprite frame drawn anywhere (item 6b's seam), which is why no
    shipped frontend can show any of this.
6b. **The sprite-selection fixture (review-8 oq3 / review-9 oq3) is blocked on a named seam, not
    on effort:** neither side can build a `KurtSprite` headlessly — the C# constructor needs the
    sealed Vulkan `Renderer` (`KurtSprite.cs:109`, `Renderer.cs:133`) and `KurtSprite::load`
    refuses a null one (`kurt_sprite.cpp:155, 256`) even though the decode only copies palette
    indices. The two candidate seams (reflect the oracle's private `Pick`, as the probe already
    does for `ObjectMotion.Roll`; or relax the renderer precondition in `load`) are written out in
    `OPEN_ITEMS.md`; pick one and the fixture is a normal batch.
6. **DONE — the object pass, all the way to the GPU**, plus review-3/-5/-6's findings on it.
   (a) **DONE — batch 22.** The missing-`TRAVSPRT.BNI` path has a committed evidence pair
   (`docs/evidence/2026-11-01-L7-sprites-missing-scene.{png,txt}`) and a gated pin
   (`tests/scripts/archive_guard_tests.cpp`). (b) **DONE — batch 27.** `dumpRunner` prints
   `mixer=`, so `mixer_.play("RUNNER", Once)` is pinned; `L7_t120_runner_{a,b}` come from the
   oracle. The probe is the scratch tree at `/tmp/mdk-pi/obj3` (a copy of `src/Mdk.Game` plus
   `Probe`); rebuild with `/tmp/mdk-pi/build_probe.sh`, then
   `DOTNET_ROOT=/tmp/kilo/dotnet10 MDK_DATA_DIR=…/MDK ./mdkprobe <level> <steps> 1 <mode>`.
   (c) **PARTLY DONE — batches 28/29.** The Citro3D backend composes each queued draw's
   `MeshDraw::world` against the eye matrix and the frame tests depth with the oracle's
   `LESS_OR_EQUAL` and per-pass write rule (`pica::depthStateForPass`, clear `0xFFFFFF`, pinned by
   `PASS PicaMatrix`). What is LEFT is the **blend** half only: no `C3D_AlphaBlend`, so
   `Pass::Blended` glass is opaque — blocked on a named cause, because the palette expansion
   forces alpha 255 and the vertex shader writes `outclr = ones`, so there is no alpha to blend
   with yet (`OPEN_ITEMS.md`). (d) The middle 173 px of the pinned frame is **not** the object
   pass; the split of the 1.240% gap, with the "do NOT re-litigate the panorama, palette,
   uniforms, projection or camera" note, is in `docs/OPEN_ITEMS.md`. (e) `FrameBlend.Lerp` is
   `Vector3.Lerp` (`fmaf(from, 1-t, to*t)`), 1 ULP from the port on one object at L3 t120, and
   `fmaf` is a libm call on devkitARM at `-mfpu=vfp`: a target codegen decision, not a
   substitution. Probe in `docs/OPEN_ITEMS.md`.

Deferred with cause: `StopFiring` inside knockDown waits for the firing slice; the flip's leftover
frame is structurally zero at the fixed 1/60 step; Rides writes the alarm countdown; consumers of
135/195/202/203/81 remain partial; the 3DS sampler fix and Kurt's muzzle flash and ride pose;
`Vector3.Lerp`'s rounding (above); the other half of `FrameBlend` (Kurt's blended feet/yaw,
`Record/Update/Turn/KurtAt`) and `TransformOf`'s driven-carrier shift, both with `Rides`. No
shipped frontend sets `Player`'s mixer (neither has an audio device) — reason in `OPEN_ITEMS.md`.
**No harness exercises `kurt_sprite.cpp`'s use of `chuteClosing()`** (see 6b). **Old-3DS timing is
UNMEASURED.**

**Owner-authorized tasks (cannot be done in a batch).** (1) Add `ralph-pi.sh`, `ralph.sh`,
`docs/KILO_RALPH_LOOP_POLICY.md`, `.kilo/agent/*`, `ralph/validator.md` and `tests/hooks/pre-commit`
to the `PROTECTED` set of `tools/runner/gates_batch_integrity.py` — that file is in its own
`PROTECTED` list, so only the owner may widen it (review-1 F1, review-2 A/E, review-9 R9-1/R9-2,
review-10 F1/F2, review-17 R34-1). Same task: the uncommitted `--each-commit` recovery moves
acceptance from one frame per **iteration** to one per **commit**, which needs an owner sign-off
line, and `--each-commit` belongs out of `required` until then (review-3 F4/F5). (2) Wire
`bash tools/runner/test_review_state.sh` into `tools/check.sh`. (3) A review is recorded `done` on
rc 0 + a non-empty report, so an `INCOMPLETE` verdict still clears the owed range (review-1 oq3).
(4) Whether the `invoked`-with-nothing-reconstructible last-resort halt should halt at all. (5) The
shipped unit restarts on exit 8 but not 9, and two restarts in one window end the loop (review-3
F4). (6) `ralph.sh:230` still carries the original unchecked `state_set` (review-4 oq1). (7) The
startup fold classifies an in-flight review as lost without reading the run dir (review-5 oq1).
(8) Whether the `Risk:` line is the builder's to write at all (review-7 oq3, review-11 oq3,
review-12 F2). (9) Whether published evidence sidecars are correctable in place. (10) Whether
`tick1`'s data-missing `SKIP` should exit non-zero. (11) AGENTS.md §11 P7 names `/tmp/kilo` as the
capture directory, but it is not writable by this account (`piagent`, uid 994), so every agent
must pick its own scratch path and say so (this batch used `/tmp/mdk-pi/b47`). (12) **CLOSED by
batch 29** — the `mdk-sdl` pi-knowledge base exists; report an *empty search* as a search that
found nothing, and record a source's unavailability (Cognee MCP is not exposed in the Pi harness).
(13) Run `python3 tools/runner/gates_batch_integrity.py --self-test` from `tools/check.sh`: it
exists and passes by hand but nothing invokes it, and it returns before the static-invariant block
(review-1 F6, review-10 F3, review-17 R34-5). (14) The diagnostic triangle in `renderer_c3d.cpp`
is `true` in the shipping build; the right shape is an SD-file switch (batch 31). (15)
**`Player::facing()`'s trig, measured and ready to land.** C# `Kurt.Facing` is `MathF.Cos/Sin`;
the port's is the trig LUT, and over the 470-step walk of `golden/L3_kurt_steps.txt` that drifts
the feet by 1.34e-05 (which is why that golden prints the feet in 64ths). Replacing the two calls
with `cosf/sinf` — the shape `util/matrix.h`'s `create_from_axis_angle` already carries an
`@vfp-exempt R6` pin for — makes the golden match at four decimals and moves the pinned frame
closer to the oracle, `1.240 / 247 / 0.1819` → **`1.095 / 247 / 0.1636`** (measured 2026-11-02,
`DISPLAY=:99`, lavapipe). A batch cannot land it: it changes the picture, and
`tests/frames/reference.sha256` is owner-protected, so the `cpp=` baseline must be regenerated
with `tests/frames/regenerate_reference.sh` by the owner, and the golden's feet re-tightened to
four decimals in the same change. Write-up in `docs/OPEN_ITEMS.md`.

## Current tree state (VERIFIED)

The full record — every invariant, every failure mode, every "do not change this" — is the appendix
to `docs/PROGRESS_LOG.md`, "Current tree state (VERIFIED) — as of batch 13", marked **superseded in
part** where batches 14-47 changed it. The headlines a batch can break:
- **Camera/renderer:** the C#'s RIGHT-HANDED `CreateLookAt` basis; Vulkan folds clip.y into the
  matrix and the sky inverse carries the flip on its LEFT side; shaders use `M * v`; Citro3D culls
  per draw with the mode cached in the **Citro3D context**. **UNVERIFIED on hardware** (P6).
- **Frame golden:** level 3, 320x240, seed 1, frame 1; `oracle=b60e72552c40…`,
  `cpp=58615fc85d02e009` (a regression baseline, NOT a fidelity reference), gap
  `1.240 / max 247 / mean 0.1819` at tolerance 0.
- **Kurt:** `kurt_sprite.{h,cpp}` is `Kurt/KurtSprite.cs`; palette-index frames (5.30 MiB) uploaded
  on first draw under `MdkCapabilities::sprite_residency_bytes` (0 desktop, 1 MiB 3DS) — UNVERIFIED
  on hardware. `Player::fall` is `Kurt.Fall` whole. **One health** (`Player::combat_`), **one
  state** (`Kurt::state_`), and live knock-counter, invulnerability-timer, flash and push fields,
  each with a golden that fails if a second copy appears: `L3_kurt_hurt`, `L3_kurt_hurtland`,
  `L3_kurt_knock`, `L3_kurt_invuln`, `L3_kurt_flashes`, `L3_kurt_push`. The push's only non-zero
  writer in the whole oracle is `ScriptRuntime::pushKurt` (opcode 248); `updateKnockDamage` hands
  `knockDown` a zero push exactly as `Kurt.Combat.cs:225` does. A frontend that re-derives the
  skull from its own clock is the duplication batch 45 removed. **The view's bank is live state
  too** (batch 53): `Player::_cameraRoll` is `Kurt.CameraRoll`, written only by `Player::update`
  at the C#'s two places (Kurt.cs:160 and :218) and read by `FollowCamera::update`; its two
  guards, its held step, its ±10 limits and its 2-degree reverse jump are pinned by
  `L3_kurt_camroll` against the oracle probe, and what the bank does to the drawn view — plus the
  wiring that makes the camera read it at all — by `CameraRollBanksTheFollowCamera`. Do not
  re-derive the bank in the camera, and do not add a `Slide`/`Follow` writer before the subsystem
  that owns it.
- **Missing-archive path:** an unparseable `TRAVSPRT.BNI` makes the frontend run the level WITHOUT
  scripts and say so, and `tick1` exits 1 rather than hand the runtime a null archive — the C#
  `Bni.Load` throws, so a null archive is a different game, not a degraded one. Never reintroduce
  a `nullptr` into the `ScriptRuntime` constructor from a shipped entry point, and do not "tighten"
  `Bni::parse` into a content check: a zero-filled archive is a legal EMPTY archive. Pinned by
  `archive_guard_tests.cpp` and `sprites_refusal_test.sh`; **`tick1`'s copy only** — the shipped
  frontend's copy at `mdk_linux/src/main.cpp:383-394` is ungated (review-10 F2).
- **Object pass:** `transform()` is `CreateScale * (rolling ? RollingBasis : Rotation) *
  CreateTranslation(...)`; `transform_at(alpha)` is what a frame draws with; `rotation()` keeps the
  libm trig and its `@vfp-exempt R6` pin; `sprites` is a REQUIRED constructor argument;
  `mdk_render_draw_mesh` **queues** and the backend buckets by `Pass` in queue order — the oracle's
  order, not a backend's (P3). `update_rolling()` runs **before** `previous_position = position`,
  where the C# does, and `mdk::create_from_axis_angle` is **bit-exact against .NET 10**. **Every movement command the C#'s `UpdateCommand` dispatches now has an arm: 1 formation, 6 chase, 15 alarm, 30 chain, 43/78/197 walk-or-fly, 61 projectile, 74 attachment, 88 forward, 229 jump — commands 1 and 6 landed in batch 48 (`followFormation`, `chase`, over the new `turnTowards`/`moveToward`), after their dispatches had set `move_command` with nothing reading it; neither fires in levels 3-8.** The speed clamps those arms run (`moveForward`, walk, fly, chase's gain and brake) use `mathMin`/`mathMax`, not `fminf`/`fmaxf`, because the oracle writes them as `MathF.Min`/`MathF.Max` and both propagate NaN; a NaN speed must stay NaN. The oracle's `Accelerate` (`ObjectMotion.cs:780-790`) is **one** function with three callers, and the port now matches that: one file-scope `accelerate(MdkObject*, float)` called by `moveForward`, the walk arm and the fly arm (batch 52, review-21 F7) — do not re-inline it, and do not fold `chase`'s clamp pair into it, because those two lines are the C#'s own `Chase` formula (`:916-917`), not a copy. All eight sites are pinned (batch 50): `ChaseSpeedClampPropagatesNaNLikeTheOracle` plus `MoveForward`/`Walk`/`Fly` `…ClampPropagatesNaNLikeTheOracle`, which feed a NaN `acceleration`/`deceleration` because `Accelerate`'s strict comparisons mean a NaN *speed* never reaches those three clamps; and `AccelerateIsOneImplementationAcrossTheArms` (batch 52) drives all three arms from identical state over 17 rows and requires each to land on the bits .NET's private `Accelerate` prints, so an arm that drifts from the shared helper fails. And the walk/fly arm's `if (move_command != 0)` guard around `detectStuck`/`handleStuck` is the oracle's own (`ObjectMotion.cs:354-358`): an object that arrives after a replan keeps its `stuck_count`, which is what opcode 231 reads — pinned by `ArrivalKeepsTheStuckCountTheOracleKeeps` on **both** arrivals (the walk arrival's skip comes from `Walk`'s own `return`, `ObjectMotion.cs:811-812`, so only the fly arrival exercises the guard) plus a non-arrival fly tick where `detectStuck` must still run (`stuck_ticks == 1`).
- **Loop harness:** fail-closed on every way a mandatory review can fail to happen, fail-*forward*
  where the obligation is recoverable; every transition goes through `state_write` (one temp file,
  one rename, a checked status); the obligation is created **before** the thing it must survive;
  nothing may sit between a verdict and the transition that records it — loop-wide. Independence is
  the resolved upstream and family, re-resolved after a fallback; the live cache is per-uid /
  per-proxy at 0600, checked on the descriptor it reads, never through a symlink, never blocked by
  a planted FIFO. State machine: `tools/runner/review_state.sh`, **177 cases**; how that suite
  lifts `ralph-pi.sh` is in "Iteration 49" and **must not be re-loosened** (batches 20-21).

## Verified on this tree (batch 54)

Narrative, the `nanclamp` probe transcript (the oracle's own printed values), the four pins,
the 5-mutant table and the `nm -u` before/after: `docs/PROGRESS_LOG.md`, "Iteration 82".
Batch 53's — `CameraRoll`, the key-window schedule, the 9-mutant table and the `math_lut.h`
negative-angle measurement — is "Iteration 81"; batch 52's — one `accelerate()`, the .NET
probe table (mode `accel`) and the six-mutant table — is "Iteration 80"; batch 51's — the
fly-arrival pin and the three-mutant guard table — is "Iteration 79"; batch 50's — the six
clamp pins and their .NET derivation — is "Iteration 78".
`make -C tests/scripts` **exit 0** (`All VM unit tests match the oracle.` + `All cases match the
oracle.`, **478 PASS / 0 FAIL**, and `./build/vm_tests` **77 PASS / 0 FAIL** — four cases more
than batch 53: `FrictionClampPropagatesNaNLikeTheOracle`,
`SegmentEntrySlabPropagatesNaNLikeTheOracle`,
`ConeFloorIsSkippedForNaNBeforeTheClampLikeTheOracle`,
`SweepHalfHeightPropagatesNaNLikeTheOracle`; and `CameraRollClampsPropagateNaNLikeTheOracle`
is now `CameraRollNaNStaysNaNWhereTheOracleThrows`, review-24 F3);
`bash tools/check.sh` **exit 0** (`CHECK: ALL GATES PASSED`:
`audit_c_with_classes 0 violations`, `gates_vfp 0 blockers`, `audit_arm11_math` contract clean,
`audit_r11_idiv` no candidates, `gates_core_parity` both targets link the same core, `gates_smdh`
SKIPPED — no built .cia, `gates_frames PASS L3_f1.bmp sha=58615fc85d02e009` — the pinned frame
unchanged, and `reference.sha256` NOT regenerated — `gates_batch_integrity` project invariants
intact); `make -C mdk_3ds` **exit 0** (`3DSX mdk.3dsx`); `cmake --build build-linux --target
mdk -j8` **exit 0**.

**What changed is shipped arithmetic, not a test.** Four sites in `script_runtime.cpp` now use
the file's NaN-propagating `mathMin`/`mathMax` instead of `fminf`/`fmaxf`, because the oracle
writes `MathF.Min`/`MathF.Max`: the friction factor (`ObjectMotion.cs:1066`), `flyProjectile`'s
`SegmentEntry` slab (`ScriptMath.cs:110-111`), `canSeeKurt`'s cone floor
(`ScriptRuntime.cs:3244`) and `sweepBox`'s half height (`ObjectMotion.cs:1138`). Finite
inputs are bit-identical — the friction pin asserts the finite row in .NET's bits
(`3FE66667,4019999A` velocity, `3D75C291,3DA3D70B` position) — and every expected NaN is the
probe's printed output, not the port's. **Two of the four cannot be pinned and both reasons
are measured, not argued**: a NaN `cone` is rejected by the `cone < HalfTurn` guard in front
of the clamp (probe `CONE x10 c45=False cNaN=True c200=True`, pinned as the *absence*), and a
NaN `leave` in the slab can never produce a miss (`enter > leave` is false for a NaN either
way), so mutants M3 and M4 are recorded unkillable rather than quietly dropped. Do not
"tidy" either back to `fminf`/`fmaxf`: the form is the oracle's. Side effect, not a perf
claim: `mdk_3ds/build/script_runtime.o` no longer has `fmaxf`/`fminf` undefined symbols, so
those clamps are no longer libm callouts on ARM11. Batch 50-53's invariants stand: the eight
speed clamps and the two `CameraRoll` ones are `mathMin`/`mathMax`, the oracle's one
`Accelerate` is one `accelerate()`, the `move_command != 0` arrival guard is killable on both
sides, and `CameraRoll` is the only writer of the view's bank.
Still NOT pinned by any golden: a shipped frontend's mixer wiring (no
frontend has an audio device, none draws `Knocked`), the fall-out variant of death, the firing step
frames, the sprite frame `pick()` selects from `chuteClosing` (6b), the flip's leftover frame, the white
flash's other writers, and feet finer than 1/64 unit (owner task 15). **The next non-owner candidates,
in order:** `scripts/debris.cpp:157-162,177-178` — the shatter's group box — is the same defect one
subsystem over, and this batch's probe measured why: the oracle there is `Debris.cs:160-166`'s
`Vector3.Min`/`Vector3.Max`, and **`System.Numerics` propagates a NaN like `MathF.Min`/`MathF.Max`
does, in both argument orders** (probe rows `V3MIN NaN,1,1 V3MAX NaN,3,4` and the two `_REV` rows),
so those `fminf`/`fmaxf` are a real (if unreachable-by-anything-in-the-shipped-data) divergence, not a
leftover to dismiss; it wants its own batch and its own pin. Then the follow camera's **blend**
(`FollowCamera.Blend` over its `Aim` triple, which needs the high-refresh frame blend `FrameBlend`'s
Kurt half already carries); then its sniper scope and top-down view. `script_runtime.cpp` has no
`fminf`/`fmaxf` left. Owner
candidates unchanged: `math_lut.h`'s `fastSin`/`fastCos` measure 2.5x their own documented error on a
**negative** argument (1.19e-05 at -10 degrees against libm; every camera and sprite angle uses that
path, so fixing it is a frame-baseline event — Iteration 81), and review-24 F1/F2, which are the
`--each-commit` and sidecar-immutability owner tasks. **No emulator run and
no Old-3DS hardware run: Old-3DS timing stays UNMEASURED and the Citro3D
picture stays UNVERIFIED (P6);** the only windowed run was the frame capture, on `DISPLAY=:99` (P7).
Frame `docs/frames/2026-11-03-nanclamp-parity.png` + `.txt` — `1.240 / 247 / 0.1819`,
byte-identical (`6646adf1dae3…`) to its parent's, and the
## Standing open items

Full list with evidence in `docs/OPEN_ITEMS.md`; the frame-record rule is in `docs/frames/README.md`
and the "the pin cannot show this" rule in `docs/evidence/README.md`. The ones that bite:

* **C3D blend state still differs from the C#** — no `C3D_AlphaBlend`, so `Pass::Blended` glass is
  opaque, blocked on a named cause: the palette expansion forces alpha 255 and `mdk_c3d.v.pica`
  writes `outclr = ones`, so there is no alpha to blend with yet. The **depth** half landed in
  batch 29; non-`Solid` batches are in **10 of level 3's 17** arenas.
* The per-eye replay **landed in batch 30** (`installEye` + `replayQueue` per eye in `end_frame`,
  `pica::eyePlan` for count/sign/clamp) — and it turned out to have been a black screen, not a
  stereo gap. The seam contract is in `mdk_core/src/render/render.h` with its twin in
  `draw_queue.h`, and `c3d_frame_test.sh` refuses both a flush and a bare draw at all six
  canvas/inset/present seams including `canvas_quad`, so a HUD batch cannot reintroduce it by
  reading either header; the HUD's shape — a queue record carrying a viewport and a zero-parallax
  flag — is in `OPEN_ITEMS.md`. `gfxSet3D` is called once at init rather than per frame.
* The sprite residency bound is compile-verified only (the 3DS frontend binds no Kurt).
* `depth_pull.cpp` is `DepthPull.Of` — Vulkan applies the pull, the C3D backend still ignores
  `depth_layer`, because the pull needs the **view position and the primitive**, which the render
  seam does not hand it (a seam question, not a substitution).
* A late-frame oracle picture needs owner-authorized tooling (`capture_oracle.sh` is pinned to
  frame 1).
* **Do not "fix" `follow_camera.cpp`'s `normalize`** — it is a reciprocal where `Vector3.Normalize`
  reads as a division, and making it a division REGRESSES the sky from 3.023% to 5.730%. The 3DS
  sampler fix stays deferred with cause (the 3DS pads index textures to POT with transparent
  padding, so a plain `C3D_TexSetWrap(REPEAT)` would tile at the padded period).
* A sidecar's "why this picture did not move / what shows the change instead" narrative **is** the
  convention (`docs/frames/README.md`); `publish_frame.sh` emits only the three measured lines and
  the narrative is appended by hand.
* The class bounds reviewers have made standing rules for: the harness that must return its counter
  on every exit path, the multi-arm gate that must count its arms, the counter that must move after
  the observable it counts, the byte-building helper that must bound and account for its own
  refusals, and the two ungated copies of the missing-archive refusal — all in `OPEN_ITEMS.md`,
  "Class bounds worth keeping".

## Verification commands

```
make -C tests/scripts clean && make -C tests/scripts   # goldens (includes sprites_refusal_test.sh)
sh tests/scripts/sprites_refusal_test.sh               # the missing-archive refusal, both arms
tests/scripts/build/tick1 dump draw 3 1                # the queued draws (also `bound`)
tests/scripts/build/tick1 dump blend 3 1               # the blended transforms (also `blendcases`)
tests/scripts/build/tick1 dump runner 7 120 -21,-13,0.73   # the hamster's run, per tick
tests/scripts/build/tick1 dump kurt 3 950             # Kurt's 950 steps: state, feet64, sounds
tests/scripts/build/tick1 dump kurtlethal 3 200       # the lethal hard landing at health 0
tests/scripts/build/tick1 dump hurt 3 220             # hurt_kurt on the one health, per step
tests/scripts/build/tick1 dump hurtland 3 340         # the hurt inside and after K_CRASHL
tests/scripts/build/tick1 dump knock 3 340            # the knock counter's decline and the landing's reset
tests/scripts/build/tick1 dump flashes 3 620          # the two flashes' fade, the skull, the beat's end
tests/scripts/build/tick1 dump push 3 360             # opcode 248: both modes, the fallback, both guards
bash tools/check.sh                                    # all gates
bash tools/runner/test_review_state.sh                 # the review-record state machine
python3 tools/runner/model_identity.py selftest --live # alias→upstream map vs the live proxy
DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds
cmake --build build-linux --target mdk -j8             # the Linux frontend
tests/frames/regenerate_reference.sh                   # intentional frame change only
```

Goldens are **470/0/0**. Tests do not track header dependencies: run `make -C tests/scripts clean`
after header changes. Never weaken a gate or test.
