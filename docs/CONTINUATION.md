# Phase 2 continuation checkpoint

Risk: high — batch 65 changes FollowCamera's NaN/signed-zero clamp behaviour and its public observation
surface (the oracle-public `Climb` plus a read-only air tilt), and adds exact oracle pins; the slice is
bounded to one camera subsystem, but the public-interface and behavioural edges make this high-risk.

Last updated: 2026-11-05 (**batch 65** — review-6 F1/F2/F3 fixed: FollowCamera's five `MathF.Min`/`MathF.Max`
sites now use the shared NaN- and signed-zero-exact helper, the climb's `-0` and the air pitch's NaN are
pinned in the oracle's bits, the arena-pitch test now carries its own fused-vs-product-sum discriminator,
and the record's 87/200 and 53/256 figures name their different expressions. The owner-authored
`docs/OWNER_DIRECTIVE_2026-11-05.md` appeared in the dirty tree during this batch and SUPERSEDES the
priority order below: the next slice is the integration sprint's Mandarine harness, not more 1-ULP
fidelity work). The record is
`docs/PROGRESS_LOG.md`, "Iteration 93"; batch 64's (review-5's two claim corrections and the
arena-pitch powf leaf) is "Iteration 92"; batch 63's (review-4's camera pins now make the pivot, roll and
arena-pitch mutants fail; the published tolerance figure corrected; `float.Lerp` and degrees-to-radians
named accurately) is "Iteration 91"; batch 62's (the follow
camera's between-step blend lands, and its scalar `Lerp` turns out to have been the wrong shape) is
"Iteration 90"; batch 61's (the R6 pin moved to the site the gate reads, the tree green again) is
"Iteration 89"; batch 60's (the record sweep, the false GNU-dialect
cause, the scoped identical-bits claim) is "Iteration 88"; batch 59's (the four codegen experiments,
the shipped-grouping replay 0/72 vs 23/72, the M1/M2 re-runs, the `.text` pair) is "Iteration 87".
Batch 58's record (the spin angle's two roundings, the 56 new spin rows, the 8-mutant table, the
`assoc` group that pins `LengthSquared`' left-assoc fold per review-27 F6) is "Iteration 86". Batch
57's record (the velocity's association, the 18 new rows, the `NORM`/`SHATTER` rows) is "Iteration
85"; batch 56's (the shatter group box's NaN, the 31-row pin) is "Iteration 84"; batch 55's (one
`math_minmax.h`, the sign-of-zero tie arm) is "Iteration 83"; batch 54's (the last four
`script_runtime.cpp` clamps, the `nanclamp` probe mode and four pins) is "Iteration 82"; batch
53's (`CameraRoll`, the key-window schedule, the `math_lut.h` negative-angle measurement) is
"Iteration 81"; batch 52's (one `accelerate()`, the `accel` probe table) is "Iteration 80"; batch
51's (the fly-arrival pin) is "Iteration 79".

## PRIORITY ORDER (owner directive 2026-10-25)

**SUPERSEDED by `docs/OWNER_DIRECTIVE_2026-11-05.md` until the owner withdraws it.** The integration
sprint's six items are now the priority: (1) the Mandarine emulator harness and its first real verdict,
(2) `Game.cs` game-flow glue, (3) 3DS input, (4) frontend wiring, (5) ndsp audio, and (6) Citro3D
alpha blend. Batch 65 was already in review-6 remediation when the directive appeared and did not start
a new fidelity slice. The old numbered list below is the historical checkpoint, not the next queue.

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

**Still owner-gated and still uncommitted in the tree** (so batches stage only their own paths and
never `git add -A`): `AGENTS.md`, `ralph/builder.md`, `ralph-pi.sh`,
`tools/runner/gates_batch_integrity.py`, `.kilo/agent/adversarial-reviewer.md`,
`ralph/MUSE_CONTEXT.md`, `docs/frames/2026-11-02-ralph-self-heal.{png,txt}`,
`docs/OWNER_DIRECTIVE_2026-11-05.md`, and `mdk_3ds/tools/emurun.sh`. Batch 65 re-verified the list at
**ten** paths; `ralph/MUSE_CONTEXT.md`, the integration-sprint directive and the emulator harness are
owner-authored files that appeared in the dirty tree during this batch. None was staged, edited or
reverted here. The self-heal frame pair is
byte-identical to the current pinned frame (md5 `6769a137…`), and committing a protected path without
`--owner-approved` makes the loop's own post-batch integrity call halt at exit 6 — review-27 F1's
failure scenario and the reason a batch must not "helpfully" bank these paths. Review-5 **A1** is now
repaired **owner-side**: `ralph-pi.sh` is mode `0755` in the worktree again and matches the index's
`100755`; the rest of that file's owner changes remain uncommitted. Review-5 **A2** is confirmed as a
fact — `tools/hooks/pre-commit` rejects a staged protected path, so a `git add -A` batch cannot land
here — and its remediation is owner-only (`MDK_OWNER_APPROVED=1`, which no batch may set); the
mitigation that has kept the loop banking work since batch 60 is the stage-only-your-own-paths rule
above. Review-5 **A3** (`--each-commit` with no `--base` validates nothing) lives in that same protected
gate and is owner-gated item 1/13 below.

**Owner-authorized tasks (cannot be done in a batch).** (1) Add `ralph-pi.sh`, `ralph.sh`,
`docs/KILO_RALPH_LOOP_POLICY.md`, `.kilo/agent/*`, `ralph/validator.md` and `tests/hooks/pre-commit`
to the `PROTECTED` set of `tools/runner/gates_batch_integrity.py` — that file is in its own
`PROTECTED` list, so only the owner may widen it (review-1 F1, review-2 A/E, review-9 R9-1/R9-2,
review-10 F1/F2, review-17 R34-1). Same task: the uncommitted `--each-commit` recovery moves
acceptance from one frame per **iteration** to one per **commit**, which needs an owner sign-off
line, and `--each-commit` belongs out of `required` until then (review-3 F4/F5) — **note the
contradiction review-29 F1 caught**: the uncommitted gate diff *adds* `"--each-commit"` to the
`required` markers for `ralph-pi.sh` (`gates_batch_integrity.py:182`), so landing that diff as it
stands makes an unsigned policy change a gate requirement. The owner must either drop the marker or
write the sign-off line; a batch may not touch either file, so this note is the record. Same task
also now
carries review-27 **F2** (`--each-commit` with no `--base` prints `BATCH INTEGRITY: PASS` over zero
commits — reproduced) and **F3** (the per-commit path is only reached after an aggregate failure,
while `ralph-pi.sh`'s header states a per-commit rule and the two frame READMEs still say "per
batch"), because both live in that same protected file and no batch may edit it. (2) Wire
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
(review-1 F6, review-10 F3, review-17 R34-5, review-30 F-C — and note the recovery path is now the
only code that can turn a gate FAIL into a PASS, which is what makes the unwired self-test matter). (14) The diagnostic triangle in `renderer_c3d.cpp`
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
four decimals in the same change. Write-up in `docs/OPEN_ITEMS.md`. (16) **The frame unit.** The
integrity gate counts PNGs and cannot tell a fresh capture from a byte-identical copy, so a
record-only batch necessarily publishes a picture that proves nothing (review-28 F5, review-29 F4,
review-30 "could not break" 10 — six identical `6769a137…` frames in a row, each honestly declared in
its sidecar). Either the pinned scene gets a second, behaviour-sensitive capture point or the gate
gets a rule about repeats; a batch may not decide which. (17) **Bind an `@vfp-exempt` pin to its
site instead of to a 25-line text window.** `audit_arm11_math.has_pin_above` (`:113-118`) honours a
pin only within `max_back = 25` lines above the call, so the length of the prose between a pin and
its code decides whether the exemption is live: batch 60 documented a pinned site, the calls moved
from 24/25 to **26** lines below the pin, and the gate went red on a comment-only diff
(review-30 F-A Critical, F-D). The hardening is to accept a pin anywhere in the enclosing function or
immediately above the site — a tightening of the gate's semantics, not a loosening, and it belongs in
its own owner-reviewed change with the corpus untouched, not in a batch that is fixing one site. (18)
**Restore the executable bit on `ralph-pi.sh`** (review-5 A1). The worktree is `0644`, the index is
`100755`, and `ExecStart=$ROOT/ralph-pi.sh` (`tools/install_ralph_pi_service.sh:71`) needs the bit, so a
unit restart right now fails `status=203/EXEC` and any `git add -A` would commit the loss. A batch cannot
do it: `chmod +x` returns `Operation not permitted` for uid 994 on a `coleshores`-owned file. Owner fix:
`chmod 755 ralph-pi.sh` (and consider the same for `ralph.sh` and `tools/ralph_pi_watchdog.sh`).

## Current tree state (VERIFIED)

The full record — every invariant, every failure mode, every "do not change this" — is the appendix
to `docs/PROGRESS_LOG.md`, "Current tree state (VERIFIED) — as of batch 13", marked **superseded in
part** where batches 14-55 changed it. The headlines a batch can break:
- **Camera/renderer:** FollowCamera's five oracle `MathF.Min`/`MathF.Max` sites use the shared
  NaN- and signed-zero-exact `mathMin`/`mathMax` pair (batch 65); `FollowCamera::climbStep` is public
  like C# `FollowCamera.Climb`, and `airPitch()` is a read-only harness observation. The C#'s
  RIGHT-HANDED `CreateLookAt` basis; Vulkan folds clip.y into the
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
  that owns it. **The between-step blend is ported** (batch 62): `FollowCamera::blend` is the C#'s
  `Blend` over its `Aim` triple (pitch-without-the-look, pivot, roll), fed by the `Aim` that
  `FollowCamera::update` now stores, and `cameraPivot`/`cameraTilt` live on `Player` because in the C#
  they are Kurt's (`Kurt.Ride.cs:32,37`), not the camera's. It has no shipped caller until
  `FrameBlend`'s Kurt half lands — that is a caller gap, not a stub: the method is complete and pinned
  by `FollowCameraBlendPlacesTheFrame`, including bit-for-bit at α=1.
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
  where the C# does, and `mdk::create_from_axis_angle` is **bit-exact against .NET 10**. **Every movement command the C#'s `UpdateCommand` dispatches now has an arm: 1 formation, 6 chase, 15 alarm, 30 chain, 43/78/197 walk-or-fly, 61 projectile, 74 attachment, 88 forward, 229 jump — commands 1 and 6 landed in batch 48 (`followFormation`, `chase`, over the new `turnTowards`/`moveToward`), after their dispatches had set `move_command` with nothing reading it; neither fires in levels 3-8.** The speed clamps those arms run (`moveForward`, walk, fly, chase's gain and brake) use `mathMin`/`mathMax`, not `fminf`/`fmaxf`, because the oracle writes them as `MathF.Min`/`MathF.Max`: both propagate NaN (a NaN speed must stay NaN) and both sort -0.0 below +0.0, and since batch 55 that pair is ONE function in `util/math_minmax.h` shared by the whole tree rather than four local copies. The oracle's `Accelerate` (`ObjectMotion.cs:780-790`) is **one** function with three callers, and the port now matches that: one file-scope `accelerate(MdkObject*, float)` called by `moveForward`, the walk arm and the fly arm (batch 52, review-21 F7) — do not re-inline it, and do not fold `chase`'s clamp pair into it, because those two lines are the C#'s own `Chase` formula (`:916-917`), not a copy. All eight sites are pinned (batch 50): `ChaseSpeedClampPropagatesNaNLikeTheOracle` plus `MoveForward`/`Walk`/`Fly` `…ClampPropagatesNaNLikeTheOracle`, which feed a NaN `acceleration`/`deceleration` because `Accelerate`'s strict comparisons mean a NaN *speed* never reaches those three clamps; and `AccelerateIsOneImplementationAcrossTheArms` (batch 52) drives all three arms from identical state over 17 rows and requires each to land on the bits .NET's private `Accelerate` prints, so an arm that drifts from the shared helper fails. And the walk/fly arm's `if (move_command != 0)` guard around `detectStuck`/`handleStuck` is the oracle's own (`ObjectMotion.cs:354-358`): an object that arrives after a replan keeps its `stuck_count`, which is what opcode 231 reads — pinned by `ArrivalKeepsTheStuckCountTheOracleKeeps` on **both** arrivals (the walk arrival's skip comes from `Walk`'s own `return`, `ObjectMotion.cs:811-812`, so only the fly arrival exercises the guard) plus a non-arrival fly tick where `detectStuck` must still run (`stuck_ticks == 1`). The **debris shatter's group box** is the same family and is now
oracle-exact too (batch 56): `Debris.cs:160-166` folds each facet's three corners with nested
`Vector3.Min`/`Vector3.Max`, which propagate a NaN, and the box is the only input to `Shot.Far`,
which scales every piece's speed and spin — so a NaN corner leaves `Far > 0` false, `f` at exactly
1.0, and every piece of the group at the full shatter speed, while the `fminf` spelling dropped
the NaN and gave a different `f` to every piece. Pinned by
`DebrisGroupBoxPropagatesNaNLikeTheOracle` on seven groups (finite, two NaN, a 129-piece NaN, two
±0, and an `assoc` group whose `far` discriminates the sum order) and on their centres, velocities
**and spin quaternions** (batch 58), whose expectations are the C# probe's own printed `SHATTER`
rows.
- **Loop harness:** fail-closed on every way a mandatory review can fail to happen, fail-*forward*
  where the obligation is recoverable; every transition goes through `state_write` (one temp file,
  one rename, a checked status); the obligation is created **before** the thing it must survive;
  nothing may sit between a verdict and the transition that records it — loop-wide. Independence is
  the resolved upstream and family, re-resolved after a fallback; the live cache is per-uid /
  per-proxy at 0600, checked on the descriptor it reads, never through a symlink, never blocked by
  a planted FIFO. State machine: `tools/runner/review_state.sh`, **177 cases**; how that suite
  lifts `ralph-pi.sh` is in "Iteration 49" and **must not be re-loosened** (batches 20-21).

## Verified on this tree (batch 65)

Batch 65 converted FollowCamera's five oracle `MathF.Min`/`MathF.Max` sites to the shared
NaN- and signed-zero-exact pair, added the oracle-public `Climb` and a read-only air-tilt observation,
and pinned the climb's `-0`, the air pitch's NaN and a camera-site fused-versus-product-sum row from
the .NET 10 oracle. It re-ran all three gates green on the tree it committed: `make -C tests/scripts`
exit 0 (`All VM unit tests match the oracle.` + `All cases match the oracle.`, **483 PASS / 0 FAIL**, no
golden moved), `bash tools/check.sh` exit 0 (`CHECK: ALL GATES PASSED`, `gates_frames` still
`L3_f1.bmp sha=58615fc85d02e009`, `reference.sha256` NOT regenerated), `make -C mdk_3ds` exit 0
(`3DSX mdk.3dsx`), and the Linux `mdk` target rebuilt `follow_camera.cpp.o` before the frame was
captured. The measurements below are batch 62 and 63's and still stand.

Narrative: `docs/PROGRESS_LOG.md`, "Iteration 93". Batch 64's (review-5's two claim corrections and the
arena-pitch powf leaf) is "Iteration 92". Batch 62's (the follow camera's blend and the
oracle's fused scalar `Lerp`) is "Iteration 90". Batch 61's (the R6 pin at the site the gate reads,
the review-30 disposition table) is "Iteration 89". Batch 60's
(the record sweep, the dialect re-measurement, the `-march` FMA measurement) is "Iteration 88";
batch 59's (the four codegen experiments, the shipped-grouping replay, the M1/M2 re-runs and the
corrected `.text` pair) is "Iteration 87". Batch 58's (the spin angle's two
roundings, the 56 spin rows, the 8-mutant table, the `SPIN`/`ASSOC` probe rows) is "Iteration 86";
batch 57's (the velocity's association, the `NORM`/`SHATTER` rows) is "Iteration 85"; batch 56's
(the group box's NaN, the 31-row pin, the ±0-unobservable measurement) is "Iteration 84"; batches
50-55 (one `math_minmax.h` and the sign-of-zero tie arm, the last four `script_runtime.cpp` clamps,
`CameraRoll` and the `math_lut.h` negative-angle measurement, the six clamp pins, the fly-arrival
pin, one `accelerate()`) are "Iteration 78-83".
`make -C tests/scripts` **exit 0** (`All VM unit tests match the oracle.` + `All
cases match the oracle.`, **483 PASS / 0 FAIL** — the new review-6 camera min/max pin, and **no golden
moved**);
`bash
tools/check.sh` **exit 0** (`CHECK: ALL GATES
PASSED`: `audit_c_with_classes 0 violations`, `gates_vfp 0 blockers`,
`audit_arm11_math` contract clean, `audit_r11_idiv` no candidates, `gates_core_parity` both targets
link the same core, `gates_smdh` SKIPPED — no built .cia, `gates_frames PASS` — the pinned frame
unchanged, and `reference.sha256` NOT regenerated —
`gates_batch_integrity` BATCH INTEGRITY: PASS); `DEVKITPRO=/opt/devkitpro
DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds` **exit 0** (`3DSX mdk.3dsx`);
`cmake --build build-linux --target mdk -j8` **exit 0** (`[100%] Built target mdk`, and it did rebuild
`follow_camera.cpp.o`, so the published frame came from a binary containing this batch).

**The scalar `Lerp` was the oracle's output but not the oracle's arithmetic, and now it is both.**
`float.Lerp` (`Single.Lerp`) is `(from*(1-t)) + (to*t)` with the first product contracted into a fused
multiply-add, so its bits are `fmaf(from, 1-t, to*t)`. Measured with the probe (`lerp1`, 200 random
triples + 56 edge rows, raw bits): the twice-rounded `from + (to-from)*t` the inherited draft shipped
differs on **87 of 200**, `from*(1-t)+to*t` twice rounded on **55 of 256**, and the fused shape on
**none**. `Lerp(-0, +0, -1)` is `-0`, which only the fused shape gives. Both ported `float.Lerp` sites
(`frame_blend::lerp(float,float,float)` and `FollowCamera::update`'s arena-pitch ease, C#
`FollowCamera.cs:104`) now use it; the oracle's other `float.Lerp` callers (`CameraRoll.Slide`,
`UpscaleImages`, `EnhancedLook`) are not ported, so there is no third site. **Target cost, measured on
the shipped object**: `mdk_3ds/build/frame_blend.o`'s `lerp(float,float,float)` is the `t>=1` compare,
one `vmul.f32`, one `vsub.f32` and a **tail `b 0 <fmaf>`** — no spill, no frame — while the `Vec3`
overload is still three `vmla.f32` (unchanged, and it stays twice-rounded on purpose: per component per
object per frame, see its note) and `follow_camera.o` gained one `bl fmaf`. Three software FMAs a frame
in a path that is not hot by AGENTS.md §2 is a different budget; if that ever changes it gets a
measurement, not an edit. Pins: 256 bit-exact rows in `system_numerics_tests.cpp`, and the blend's own
8 steps × 5 α × (pos, forward, up) in `vm_tests.cpp` at measured worst relative deltas of
**1.29377659e-05** for the view rows and **1.50669503e-05** for the +X place rows (tolerance 2e-05,
so 1.55x headroom over the view rows and 1.33x over the worst (+X place) row; the LUT trig is what
spends it), with α=1 and the first frame pinned bit-for-bit.
Batch 63 adds the missing discriminating pins: a second oracle scenario moves the pivot from 4.5 to
7.25 and the bank from 0 to 0.375 degrees, and the arena pitch's own bits after five targets pin the
fused ease exactly. Dropping the pivot, dropping the roll or restoring the twice-rounded ease each
now fails `vm_tests`.

**VFPv2's VMLA is UNFUSED, and that is the fact the record had backwards.** `mdk_3ds/Makefile`
used to claim VMLA.F32 was a fused single-rounded MAC whose rounding was "UNMEASURED on Old-3DS
hardware", and review-28 F2 used that to conclude the shipped 3DS spin diverges from the oracle.
Measured on the shipped flags: the default is `-ffp-contract=fast` on devkitARM 15.2 **and** host
gcc 13.3 (`-Q --help=common`) — and **not** because of the GNU dialect, which batch 59 then got
wrong and review-29 F2 corrected: `-std=c++17`, `-std=gnu++17` and `-std=c++20` all report `fast`
on both compilers, so no dialect makes the flag bite here. `debris.cpp.o` is **byte-identical** at
default/`=off`/`=fast`, an explicitly twice-rounded `float t=a*b; return c+t;` still lowers to ONE
`vmla.f32` under `=off`, the same source at `-mfpu=vfpv4` becomes `vfma.f32` only under `=fast`, and
`__builtin_fmaf` at `-mfpu=vfp` is `b fmaf`. VMLA rounds its product, so forming it is
value-preserving **for normal-range intermediates** (the target runs FPSCR.FZ per R1, the host does
not — a denormal product would flush on the target, and review-30 F-E proved that cannot happen here:
the smallest non-zero shatter falloff `1.0f - r` can produce is `2^-24 = 5.96e-8`, twice the
`3.05e-8` a denormal would need) and the flag has nothing to gate. Replaying the shipped `randomSpin` grouping with a
rounded product gets **0 of 72** pinned components wrong; the fused model gets **23 of 72 on 12 of
18 pieces** — a scratch model, not a committed harness, and review-29 F7 is right that it cannot be
re-derived from this tree today. Do NOT
"fix" the four expressions in `quatFromYawPitchRoll` because they lower to `vmla`/`vnmls`, and do
NOT resurrect "the two targets disagree" for `frame_blend.cpp`'s `lerp`: both targets twice-round
it identically **for the flags both builds use** — neither host build sets `-march`, and the
x86-64 baseline has no FMA (measured: `-march=native` on this host emits 4 `vfmadd*` for that
function, the default 0), so that gap is port-vs-oracle on both. Pinning `-ffp-contract=off`
host-side is the named `OPEN_ITEMS.md` follow-up. **What IS unmeasured on the target is the
libm leaf** — the shipped `sinf`/`cosf` come from newlib and only glibc's were checked against
`MathF.Sin`/`MathF.Cos`; named in `docs/OPEN_ITEMS.md`, unclosable here (no newlib sources, no
`qemu-arm`), and the first place to look if a capture ever shows a trig divergence.

**The debris shatter is oracle-exact end to end — host harness and shipped object.** Settled facts
a later batch must not undo, each pinned by `DebrisGroupBoxPropagatesNaNLikeTheOracle`, whose
expectations are the C# probe's own printed `SHATTER` rows:
- `degreesToRadians` is `(degrees * kPi) / 180.0f`, because .NET's `float.DegreesToRadians` is
  `degrees * (float)Math.PI / 180f` — **two** roundings. The single-rounding factor spelling is 1 ULP
  low on 8 of the first 12 angles and reproduces only 4/12; the port's reproduces 12/12 and then
  matches the oracle's `s=` bits on all 54 pieces. Do not "tidy" it into a precomputed `pi/180`
  factor or a reciprocal divide: both are mutants the pin kills (M1/M3/M4), and the pre-fix code
  fails **93** spin assertions and 0 velocity ones — no golden prints spin, which is why a 1-ULP
  angle error sat here for dozens of batches, and `w` hid it because a 1-ULP angle move is ~0.3 ULP
  of a near-1 cosine product while `x`/`y`/`z` move 1-2 ULP. The trig and the quaternion were both
  ruled out by measurement first (glibc `sinf`/`cosf` are bit-identical to `MathF.Sin`/`MathF.Cos`
  on these angles; the port's `quatFromYawPitchRoll` product shape scores 17/17 against .NET).
- `Debris.cs:210,219` write `Direction * Speed * f`, which C# associates LEFT as `(v * Speed) * f`,
  and the port writes that; do not "tidy" it into a pre-multiplied scalar, because an axis-aligned
  direction hides the difference (`1 * x` is exact) and only `oblique`/`oblique2`/`radial` can see it.
- `vnormalize`'s `a / sqrtf(sum)` is bit-exact against `Vector3.Normalize` on 16/16 probe vectors
  including zero (NaN), a NaN component, an overflowing sum (Inf → 0), a denormal sum and `-0`
  components; the reciprocal spelling `a * (1/len)` matches only 9/16, so do not "optimise" it.
- `vdot`'s fold is LEFT-assoc, pinned by the `assoc` group (3DAF1718, not 3DAF1719 — review-27 F6).
- Measured footprint, not a perf claim (`arm-none-eabi-size mdk_3ds/build/debris.o | tail -1`):
  `.text` **9697 → 9717** under the shipped flags, `vcvt` 15/15, `nm -u` identical (the divide is a
  hardware `vdiv.f32`).

**There is no `fminf`/`fmaxf` left anywhere in the port** (batch 56): not in `mdk_core/src`,
`mdk_3ds/src` or `mdk_linux/src`, and `arm-none-eabi-nm mdk_3ds/mdk.elf | grep -iE 'fmin|fmax'` is
empty. FollowCamera's five oracle `MathF.Min`/`MathF.Max` sites now use the ONE pair in
`util/math_minmax.h` (batch 65), which is NaN-propagating, -0.0-aware, and reads the sign off the bits
with an **integer** compare. The remaining `std::min`/`std::max` candidates outside this camera are
item 7 and the `docs/OPEN_ITEMS.md` list, not silently-equivalent sites. Do not put a `fminf` back: its
±0 tie order is implementation-defined
(glibc 2.39 returns its **second** operand, matching .NET in one argument order and not the other;
the same source with visible constants folds to a **third** answer at `-O2`). The box itself is
pinned from both ends by `inside`/`near_max`, which pin which side each axis reads its far corner
from.

Still NOT pinned by any golden: a shipped frontend's mixer wiring (no frontend has an audio
device, none draws `Knocked`), the fall-out variant of death, the firing step frames, the sprite
frame `pick()` selects from `chuteClosing` (6b), the flip's leftover frame, the white flash's
other writers, and feet finer than 1/64 unit (owner task 15). **The piece spin quaternion is no
longer one of them** (batch 58). **The follow camera's blend is ported and pinned (batch 62) but has
no shipped caller**: the C# calls it from `Viewer.cs:619` with `FrameBlend`'s blended Kurt pose, and
that half of `FrameBlend` (`Record/Update/Turn/KurtAt`, `Kurt.Teleports`, the carrier) is deferred
with Rides, so the frontends still draw at `Player::feet` with the step's camera. **The next
non-owner candidates are the follow camera's sniper scope and its top-down view** (both are branches
`Blend` and `Update` currently take the false side of), then `FrameBlend`'s Kurt half with Rides.
Owner candidates unchanged:
`math_lut.h`'s
`fastSin`/`fastCos` measure 2.5x their own documented error on a **negative** argument (1.19e-05
at -10 degrees against libm; every camera and sprite angle uses that path, so fixing it is a
frame-baseline event — Iteration 81), and the `--each-commit`, sidecar-immutability,
`PROTECTED`-set and checked-in-probe tasks. **A sixteenth owner ask from review-28 F5:** the frame
gate counts PNGs, so it cannot tell a re-published copy from a capture — 12 consecutive pairs share
`6646adf1dae3…` — and the fix is either to require the new PNG to differ from its parent or to
accept an explicit `visual delta: none, justification: …` field; both live in protected frame
tooling, so a batch can only keep writing the honest sidecar narrative (this one does). **No emulator run and no Old-3DS hardware run:
Old-3DS timing stays UNMEASURED and the Citro3D picture stays UNVERIFIED (P6);** the only
windowed run was the frame capture, on `DISPLAY=:99` (P7). Frame
`docs/frames/2026-11-05-review-4-caps.png` + `.txt` (md5 `6769a137…`, unchanged, and the sidecar
says why: at frame 1 the arena pitch is already at its target, while the pivot/roll blend path has
no shipped frontend caller yet).

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

The suite prints **483 PASS / 0 FAIL** (`make -C tests/scripts`); `./build/vm_tests` alone prints **82**. Tests do not track header dependencies: run `make -C tests/scripts clean`
after header changes. Never weaken a gate or test.
