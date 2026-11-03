# Phase 2 continuation checkpoint

Risk: high — batch 44 lands the knock-down: a new BSP collision query (`ArenaSpace::floor`,
the C#'s `Floor` — arenas around **both** ends, the nearest crossing), two new state
transitions (`Player::knockDown` into `Knocked`, and the `Knocked`→`GetUp`→`Still` arms of the
combat state machine), and a per-frame change in `Player::update` (the trigger that enters
them, plus the walk-input suppression while they hold). It is not one field the way batches
40-43 were: state transitions and a collision query are two of the things the risk rubric names
as high. Both are pinned by new oracle goldens (six mutants built and run), and no shipped
frontend can show them yet because none draws Kurt's `Knocked` frame (item 6b).

Last updated: 2026-11-03 (**batch 44** — the knock-down, the thing batches 40-43 each ended by
naming as "NEXT, same family". Review-14 raised **no Critical or High finding** and verdicted
COMPLETE, so nothing needed remediating: its R13-C (the `kurt.h` comment that still listed
`KnockdownInvulnerability` as belonging to a later slice, which this batch was about to walk
into) is fixed, and R13-A/B/D/E are owner-owned — the half-applied harness slice, the
`--each-commit` ratification, the un-invoked `--self-test`, and committing the oracle probe; see
`docs/PROGRESS_LOG.md`, "Iteration 72". Previous narratives are one "Iteration N" section back
each: batch 43 = 71, 42 = 70, 41 = 69, 40 = 68, 39 = 67, 38 = 66, 37 = 65,
36 = 64, 35 = 63, 34 = 62, 33 = 61, 32 = 60.

Goldens are **455/0/0**: batch 44 added `golden/L3_kurt_knockdown.txt` (341 lines, sha256
`7e1862ea…`, the oracle probe's new `knockdown` mode, byte-identical to
`tick1 dump knockdown 3 340`) and `golden/L3_floor_probes.txt` (49 lines, sha256 `a9e38021…`,
the probe's `floor` mode, which does separate `floor` from `floorCrossing` on two of the
level's 48 segments), batch 43 added `golden/L3_kurt_invuln.txt` (341 lines, sha256
`fb00137b…`, the oracle probe's new `invuln` mode, byte-identical to
`tick1 dump invuln 3 340`), batch 42 `golden/L3_kurt_knock.txt` (341 lines, sha256
`5c2c3091…`), batch 41 `golden/L3_kurt_hurtland.txt` (341 lines, sha256 `9e1b4832…`) and
batch 40 `golden/L3_kurt_hurt.txt` (221 lines, sha256 `35d78b3d…`); no older golden moved.
The frame
is byte-unchanged across batches 31-43 because no renderer behaviour was touched. Still
owner-gated, and still uncommitted in the tree: the owner's harness work (`AGENTS.md`,
`ralph/builder.md`, `ralph-pi.sh`, `gates_batch_integrity.py`,
`docs/frames/2026-11-02-ralph-self-heal.{png,txt}`), so batches keep staging only their own
paths and never `git add -A`. **Review-10 F1, F2 and F3 and F6 are that same hazard under
new numbers** — the `--each-commit` recovery that replaces the guard's fail-closed path,
the green tree that depends on two uncommitted files, the un-invoked `--self-test`, and the
`--each-commit`-without-`--base` no-op. Review-12 B re-confirmed the clone half by execution
and **corrected review-11 R11-2's premise**: at HEAD neither file mentions `--each-commit`
(both greps → 0), so HEAD is self-consistent and the live hazard is committing only one of
the two dirty owner files, not a deadlocked tree. All four remain owner task (1)/(13).

## PRIORITY ORDER (owner directive 2026-10-25)

1. **DONE — mixer call-sites** (`ObjectMotion.cs:133`, `ObjectBehaviors.cs:90-115`).
2. **DONE — `Debris.Shatter` + opcodes 137/138/139.** BreakUp/Trail/Updraft await Effects.
3. **DONE for the C# oracle — opcode conversion.** 211 dispatches pinned in
   `docs/OPCODE_COVERAGE.md`; the remaining 27 each need a deferred subsystem. Do not
   blind-port them or read them off `MDKD3D.EXE`.
4. **DONE — P5.0 frame-golden harness** (`gates_frames.py` is gate 7; proven to fail).
5. **DONE — review-30's 3DS texture-residency MAJOR** (decode at load, upload on first
   draw under a named 1 MiB budget, restore on eviction).
6a. **DONE — the combat half of `Kurt` is live state, not write-only fields (batches 40-44).**
    `Player` owns the combat half (`Player::combat_`) and the **health** lives in it alone
    (`ScriptRuntime::kurt()` returns `player_->combat()`; a runtime with no Player falls back to
    its own `unboundKurt_`) — pin `golden/L3_kurt_hurt.txt`, which prints `hp=` off the entity and
    `vmhp=` off the runtime and fails at the first hurt if the runtime keeps a copy. The **state**
    lives in `Kurt` alone (`Kurt::setState`, written only by `Player::setState`; `Player::_state` is
    gone), which is what lets `hurt()`'s `Dead|Knocked|GetUp|HardLand` guard fire at all — pin
    `golden/L3_kurt_hurtland.txt` (a hurt at step 220 *inside* the 104-step K_CRASHL window keeps
    `hp=90`; one at 300 after it moves it to 86). The **recent-damage counter** declines
    (`Kurt::declineKnockDamage`: clamp into `[0, KnockdownDamage]`, drain 2/s) and a hard landing
    zeroes it where `Kurt.HardLanding` does — pin `golden/L3_kurt_knock.txt`. The **invulnerability
    timer** declines (`Kurt::declineInvulnerability`, `Kurt.cs:139`, ahead of every early return
    `Player::update` has, so a corpse's timer runs down too) — pin `golden/L3_kurt_invuln.txt`.
    And **batch 44 landed the knock-down itself**: `ArenaSpace::floor` (the C#'s `Floor`, arenas
    around both ends, nearest crossing — NOT the port's `floorCrossing`, which is `FloorCrossing`:
    Kurt's arenas around `from`, first hit), `Player::knockDown` (counter spent, 3 s
    invulnerable, chute closed, `Knocked`), `Player::updateKnockDamage` whole (the three guards,
    on a floor or in the air within 13 units with a slam to -64, and the decline as its tail), the
    `Knocked`→`GetUp`→`Still` arms of `updateState` (K_BANG 25 frames, push cleared at the flip,
    K_BFLIP 27, and the GetUp arm returning false so the ordinary selection runs on the step he
    stands), and the walk-input suppression while they hold — pins `golden/L3_kurt_knockdown.txt`
    (six mutants run) and `golden/L3_floor_probes.txt` (the two probes printed side by side over
    48 segments; they part company on the two long ones through HMO_9/CHMO_8). **NEXT, same
    family, in order:** the combat fades (`FadeFlashes`, `Kurt.Combat.cs:203`) are unported, so
    nothing fades `hurtFlash_` or `whiteFlash_` — opcode 215's screen flash and `damage()`'s red
    flash both stay up forever, and the death transition's `HurtFlash = 0` and `UpdateDeath`'s
    skull rise (`SkullFade`, and the `Died` event behind it) are the other half of that field;
    `push_kurt` (opcode 248) is the only thing that can give `knockDown` a non-zero push, so the
    push and its clearing at the flip are unpinned; `StopFiring` waits for the firing slice; and
    `Knocked`/`GetUp` have no sprite frame drawn anywhere (item 6b's seam), which is why no shipped
    frontend can show any of this.
6b. **The sprite-selection fixture (review-8 oq3 / review-9 oq3) is blocked on a named
    seam, not on effort:** neither side can build a `KurtSprite` headlessly — the C#
    constructor needs the sealed Vulkan `Renderer` (`KurtSprite.cs:109`,
    `Renderer.cs:133`) and `KurtSprite::load` refuses a null one (`kurt_sprite.cpp:155,
    256`) even though the decode only copies palette indices. The two candidate seams
    (reflect the oracle's private `Pick`, as the probe already does for
    `ObjectMotion.Roll`; or relax the renderer precondition in `load`) are written out in
    `OPEN_ITEMS.md`; pick one and the fixture is a normal batch.
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
   120 1 runner -21,-13,0.73` (and `-26,-18,0.73`) re-derives both files; the Kurt modes are
   `mdkprobe 3 950 1 kurt`, `… 3 200 1 kurtlethal` and `… 3 220 1 hurt`. Its `src/**.cs` differs from the
   read-only oracle only in the known print-only instrumented files. (c) **PARTLY DONE — batches 28
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

Deferred with cause: 248 `push_kurt` is the only thing that can hand a knock-down a push, so
the push and its clearing at the flip stay unpinned (batch 44 landed the knock-down itself, and
its caller passes zero); Rides writes the alarm countdown;
consumers of 135/195/202/203/81 remain partial; the 3DS sampler fix and Kurt's muzzle
flash and ride pose; `Vector3.Lerp`'s rounding (above); the other half of
`FrameBlend` (Kurt's blended feet/yaw, `Record/Update/Turn/KurtAt`) and `TransformOf`'s
driven-carrier shift, both with `Rides`. **No harness exercises `kurt_sprite.cpp`'s use of
`chuteClosing()`** (review-8 oq3 / review-9 oq3): batch 38 pins the field the sprite selector
reads and batch 39 named the seam that blocks picking the closing sprite frame — see 6b.
Kurt's footsteps and `LAND` sound are **CLOSED by batch 36**, the chute stop's hard-landing
and death coverage by batch 37, and the `closing=`/`hp=` columns by batch 38; what is left of
that item is that **no shipped frontend sets `Player`'s mixer** (neither has an audio device,
and `ScriptRuntime` owns the only mixer and hands it out `const`), with the reason in
`OPEN_ITEMS.md`. All with evidence in `OPEN_ITEMS.md`. **Old-3DS timing is UNMEASURED.**

**Owner-authorized tasks (cannot be done in a batch).** (1) Add `ralph-pi.sh`, `ralph.sh`,
`docs/KILO_RALPH_LOOP_POLICY.md`, `.kilo/agent/*`, `ralph/validator.md` and
`tests/hooks/pre-commit` to the `PROTECTED` set of `tools/runner/gates_batch_integrity.py`
— that file is in its own `PROTECTED` list, so only the owner may widen it (review-1 F1,
review-2 A/E, review-9 R9-1/R9-2, review-10 F1/F2: a builder can rewrite the guard's
fail-closed path, the reviewer's own instruction set, or the sole registration of the
Citro3D frame gate and every gate stays green; and the tree is green only because two
uncommitted files pair up — a clean `HEAD` fails its own gate). Same task: the uncommitted
`--each-commit` recovery moves acceptance from one frame per **iteration** to one frame per
**commit** (N commits ⇒ N frames), which needs an owner sign-off line rather than a
builder's assumption, and `--each-commit` belongs out of `required` until that decision
(review-3 F4/F5). (2) Wire `bash tools/runner/test_review_state.sh` into `tools/check.sh`
as a gate line; `check.sh` is owner-protected, so the state machine is checked only by hand
(batch 12). (3) A review is recorded `done` on rc 0 + a non-empty report, so an `INCOMPLETE`
verdict still clears the owed range; whether it should is a policy change to `review_outcome`
(review-1 oq3). (4) Whether the `invoked`-with-nothing-reconstructible last-resort halt
should halt at all (review-3 oq1). (5) The shipped unit restarts on exit 8 (BLOCKED) but not
9, and two restarts in one review window end the loop for good (review-3 F4, batch 17).
(6) `ralph.sh:230` still carries the original unchecked `state_set` and `8cc37ad` reverts
`ralph.sh` deliberately, so "one writer, one refusal" is a property of `ralph-pi.sh` only
(review-4 oq1). (7) The startup fold classifies an in-flight review as lost **without
reading the run dir**, where `review-$ITER.rc` and `review-$ITER.md` already say whether the
reviewer finished (review-5 oq1, re-asked by review-6 oq1). (8) Whether the `Risk:` line is
the builder's to write at all: the builder authors the input to its own review policy
(review-7 oq3, review-11 oq3, review-12 F2). (9) Whether published evidence sidecars are
correctable in place (`docs/evidence/README.md` says never edit a published pair).
(10) Whether `tick1`'s data-missing `SKIP` should exit non-zero, i.e. whether "the golden
suite could not run" is a gate failure on hosts without the licensed tree. (11) AGENTS.md
§11 P7 names `/tmp/kilo` as the capture directory, but it is not writable by this account
(`piagent`, uid 994), so every agent must pick its own scratch path and say so (both this
batch and review-11 hit it). (12) **CLOSED by batch 29** — the `mdk-sdl` pi-knowledge base
exists, so the mandatory prior-solution lookup can run; report an *empty search* as a search
that found nothing, and record a source's unavailability (Cognee MCP is not exposed in the
Pi harness). (13) Run `python3 tools/runner/gates_batch_integrity.py --self-test` from
`tools/check.sh`: it exists and passes by hand but nothing invokes it, and it returns before
the static-invariant block, so it prints PASS over invariants it never checked (review-1 F6,
review-10 F3). (14) The diagnostic triangle in `renderer_c3d.cpp` is `true` in the shipping
build and can only be turned off by a rebuild; the right shape is an SD-file switch (batch
31). (15) **`Player::facing()`'s trig, measured and ready to land.** C# `Kurt.Facing` is
`MathF.Cos/Sin`; the port's is the trig LUT, and over the 470-step walk of
`golden/L3_kurt_steps.txt` that drifts the feet by 1.34e-05 (which is why that golden prints
the feet in 64ths). Replacing the two calls with `cosf/sinf` — the shape `util/matrix.h`'s
`create_from_axis_angle` already carries an `@vfp-exempt R6` pin for — makes the golden match
at four decimals and moves the pinned frame closer to the oracle, `1.240 / 247 / 0.1819` →
**`1.095 / 247 / 0.1636`** (measured 2026-11-02, `DISPLAY=:99`, lavapipe). A batch cannot
land it: it changes the picture, and `tests/frames/reference.sha256` is owner-protected, so
the `cpp=` baseline has to be regenerated with `tests/frames/regenerate_reference.sh` by the
owner. Whoever lands it should re-tighten the golden's feet to four decimals in the same
change. Write-up in `docs/OPEN_ITEMS.md`.

## Current tree state (VERIFIED)

The full record — every invariant, every failure mode, every "do not change this" — is the
appendix to `docs/PROGRESS_LOG.md`, "Current tree state (VERIFIED) — as of batch 13", marked
**superseded in part** where batches 14-25 changed it. The headline:
- **Camera/renderer:** the C#'s RIGHT-HANDED `CreateLookAt` basis; Vulkan folds clip.y into the
  matrix and the sky inverse carries the flip on its LEFT side; shaders use `M * v`; Citro3D culls
  per draw with the mode cached in the **Citro3D context**. **UNVERIFIED on hardware** (P6).
- **Frame golden:** level 3, 320x240, seed 1, frame 1; `oracle=b60e72552c40…`,
  `cpp=58615fc85d02e009` (a regression baseline, NOT a fidelity reference), gap
  `1.240 / max 247 / mean 0.1819` at tolerance 0.
- **Kurt:** `game/kurt_sprite.{h,cpp}` is `Kurt/KurtSprite.cs`; palette-index frames
  (5.30 MiB measured) uploaded on first draw under `MdkCapabilities::sprite_residency_bytes`
  (0 desktop, 1 MiB 3DS) — UNVERIFIED on hardware (P6). `Player::fall` is `Kurt.Fall` whole.
  **`Player` owns the one health** (`Player::combat_`, `Kurt::health_`), and
  `ScriptRuntime::kurt()` is that object when a Player is bound; a Player that grew its own
  second health, or a runtime that kept a copy, is caught by `golden/L3_kurt_hurt.txt`.
  **And `Kurt::state_` is the one state** (`Player::_state` is deleted since batch 41 and
  `Player::state()` delegates to the combat half), which is what makes `hurt()`'s
  `Dead|Knocked|GetUp|HardLand` guard fire at all; a second copy is caught by
  `golden/L3_kurt_hurtland.txt`. **And the recent-damage counter is live state, not a write-only
  field**: `Kurt::declineKnockDamage` runs from `Player::update` at the C#'s call site and
  `Player::land` zeroes it the way `Kurt.HardLanding` does, pinned by
  `golden/L3_kurt_knock.txt` and three `vm_tests` cases.
  **When `TRAVSPRT.BNI` cannot be parsed the frontend runs the level WITHOUT scripts and says
  so, and `tick1` exits 1 rather than hand the runtime a null archive** — the C# `Bni.Load`
  throws, so a null archive is a different game, not a degraded one. Do not reintroduce a
  `nullptr` into the `ScriptRuntime` constructor from a shipped entry point, and do not
  "tighten" `Bni::parse` into a content check: a zero-filled archive is a legal EMPTY archive
  and the C# can produce a missing entry, so `itemsAnimation` returning `nullptr` for a
  missing entry is faithful. The parse guard is pinned by `tests/scripts/archive_guard_tests.cpp`
  (offset and size, not just `has()`, since batch 23; the **name-field width** since batch 24 via
  `testFullNameFillsTheField`, whose 12-char no-NUL name is the only fixture where reading the
  field as 8 cannot pass; and the **fixture helper's own bound** since batch 25 via
  `testOverlongFixtureNameIsRefused`, and a refusal that no case accounted for fails the suite
  since batch 26), the **refusal branch itself** by `tests/scripts/sprites_refusal_test.sh` — a
  4-byte stub must exit non-zero with the message and an 8-byte legal empty archive must exit 0
  without it, same argv, no ROM data — and the scene is pictured in
  `docs/evidence/2026-11-01-L7-sprites-missing-scene.png`. That gate covers **`tick1`'s copy
  only**; the shipped frontend's copy at `mdk_linux/src/main.cpp:383-394` is ungated (Standing
  open items, review-10 F2).
- **Object pass:** `transform()` is `CreateScale * (rolling ? RollingBasis : Rotation) *
  CreateTranslation(...)` via `mdk::multiply`; `transform_at(alpha)` is what a frame draws with;
  `rotation()` keeps the libm trig and the `@vfp-exempt R6` pin; `roll` has a writer (`motionBank`
  is `ObjectMotion.Bank`); the Runner is `ObjectBehaviors.UpdateRunner`, and **`sprites` is a
  REQUIRED constructor argument** (since batch 11 no shipped entry point can pass `nullptr`).
  `mdk_render_draw_mesh` **queues** and the backend buckets by `Pass` in queue order — the oracle's
  order, not a backend's (lifetime in `render/draw_queue.h`, P3). `update_rolling()` runs **before**
  `previous_position = position`, where the C# does, and `mdk::create_from_axis_angle` is
  **bit-exact against .NET 10**.
- **Loop harness:** fail-closed on every way a mandatory review can fail to happen, and fail-*forward*
  where the obligation is recoverable. Every transition goes through `state_write` (one temp file, one
  rename, **a checked status**), and `state_set` is the single-key spelling of that SAME writer. **The
  obligation is created before the thing it must survive** — dispatch persists the in-flight range and
  `invoked` before the reviewer is spawned. **And nothing may sit between a verdict and the transition
  that records it — loop-wide.** Independence is the resolved upstream and family, re-resolved after a
  fallback; the live cache is per-uid/per-proxy at 0600, checked on the descriptor it reads, never
  through a symlink, never blocked by a planted FIFO. State machine: `tools/runner/review_state.sh`,
  **177 cases**; how that suite lifts `ralph-pi.sh` is in "Iteration 49" and **must not be
  re-loosened** (batches 20-21).

## Verified on this tree (batch 44)

All gates re-run from a clean invocation after the last edit; the full narrative, the probe
recipe and the six mutant results are in `docs/PROGRESS_LOG.md`, "Iteration 72". Goldens
**exit 0** (`All cases match the oracle.`, **455 PASS / 0 FAIL / 0 SKIP**); `tools/check.sh`
**exit 0** (`CHECK: ALL GATES PASSED`; `audit_c_with_classes 0 violations`, `gates_vfp 0
blockers`, `audit_arm11_math contract clean`, `audit_r11_idiv no candidates`,
`gates_core_parity` clean, `gates_smdh` SKIPPED, `gates_frames PASS L3_f1.bmp
sha=58615fc85d02e009`, byte-identical to the pinned `cpp=`, so `reference.sha256` was NOT
regenerated; integrity PASS); `make -C mdk_3ds` **exit 0** (`3DSX mdk.3dsx`, no warnings);
`cmake --build build-linux --target mdk` **exit 0**. What is new:
`golden/L3_kurt_knockdown.txt` (sha256 `7e1862ea…` — hurt 5 on the pad knocks him down at
step 21 with `invuln=3.0000 knock=0.0000`, forward held 25..60 does not move him, a hurt at 40
is refused by the `Knocked` arm with the timer written to 0, K_BANG flips at 70 and `Still`
returns at 124, and a hurt at 156 six units above the pad slams him to `vert=-65.0665` and
knocks him down mid-air) and `golden/L3_floor_probes.txt` (sha256 `a9e38021…` — `floor` and
`floorCrossing` over 48 segments, agreeing on 46 and parting on the two long ones). Both are
the oracle probe's own output, byte-identical to the `tick1` twin, with the probe's older
`invuln`/`knock`/`hurt` modes re-diffed against their committed goldens first. Six mutants,
all killed (see the log table). What the kurt goldens still do NOT pin: any shipped frontend's
wiring (no frontend sets the mixer or draws `Knocked`), the fall-out variant of death
(unreachable without a pinned solid arena), the firing step frames, the sprite frame `pick()`
selects from `chuteClosing` (see 6b), the knock-down *push* (only `push_kurt` can supply one),
the flip's leftover frame (structurally 0 at a fixed 1/60 step), and the feet finer than 1/64
unit (`Player::facing()`'s LUT trig; owner task 15). **No emulator run and no Old-3DS hardware
run: Old-3DS timing stays UNMEASURED and the Citro3D picture stays UNVERIFIED (P6);** the only
windowed run was the frame capture, on `DISPLAY=:99` (P7). Frame
`docs/frames/2026-11-03-kurt-knockdown.png` + `.txt` — `1.240 / 247 / 0.1819`, byte-identical
to its predecessors, because no renderer behaviour changed. No gate was pinned, relaxed or
edited.

## Standing open items

Full list with evidence in `docs/OPEN_ITEMS.md`; the frame-record rule is in
`docs/frames/README.md` and the "the pin cannot show this" rule in `docs/evidence/README.md`. The
ones that bite: **C3D blend state still differs from the C#** (no `C3D_AlphaBlend`, so
`Pass::Blended` glass is opaque) and it is blocked on a named cause — the palette expansion forces
alpha 255 and `mdk_c3d.v.pica` writes `outclr = ones`, so there is no alpha to blend with yet; the
**depth** half of that item landed in batch 29 (`LESS_OR_EQUAL`, the per-pass write rule, clear
`0xFFFFFF`, pinned by `PASS PicaMatrix`), and non-`Solid` batches are in **10 of level 3's 17**
arenas; the per-eye replay **landed in batch 30** (`installEye` + `replayQueue` per eye in
`end_frame`, `pica::eyePlan` for the count/sign/clamp) — and it turned out to have been a black
screen, not a stereo gap. Two follow-ups it left: the seam contract is in
`mdk_core/src/render/render.h` and its twin in `draw_queue.h` now says the same thing (review-3 F6),
and `c3d_frame_test.sh` refuses **both a flush and a bare draw at all six canvas/inset/present seams
including `canvas_quad`** (review-3 F1), so a HUD batch cannot reintroduce it by reading either
header; the HUD's actual shape — a queue record carrying a viewport and a zero-parallax flag, per
the two vault patterns — is in `OPEN_ITEMS.md`; `gfxSet3D` is called once at init rather than per
frame from the slider, which is the common homebrew pattern but not what Forsaken does); the sprite
residency bound is compile-verified only (the 3DS frontend binds no Kurt); `depth_pull.cpp` is
`DepthPull.Of` — Vulkan applies the pull and the C3D backend still ignores `depth_layer`, now for a
different reason: the depth test exists, but the pull needs the **view position and the primitive**,
which the render seam does not hand the C3D backend (a seam question, not a substitution); a
late-frame oracle picture needs owner-authorized tooling (`capture_oracle.sh` is pinned to frame 1).
**Do not "fix" `follow_camera.cpp`'s `normalize`** (a reciprocal where `Vector3.Normalize` reads as a
division; making it a division REGRESSES the sky from 3.023% to 5.730%), and the 3DS sampler fix stays
deferred with cause (the 3DS pads index textures to POT with transparent padding, so a plain
`C3D_TexSetWrap(REPEAT)` would tile at the padded period). Answered rather than left implicit: a
sidecar's "why this picture did not move / what shows the change instead" narrative **is** the
convention (`docs/frames/README.md`); `publish_frame.sh` emits only the three measured lines and the
narrative is appended by hand. The class bounds a reviewer has made standing rules for the next batch -- the harness that must return its counter on every exit path, the multi-arm gate that must count its arms, the counter that must move after the observable it counts, the byte-building helper that must bound and account for its own refusals, and the two ungated copies of the missing-archive refusal -- are in `docs/OPEN_ITEMS.md`, "Class bounds worth keeping".

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
bash tools/check.sh                                    # all gates
bash tools/runner/test_review_state.sh                 # the review-record state machine
python3 tools/runner/model_identity.py selftest --live # alias→upstream map vs the live proxy
DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds
cmake --build build-linux --target mdk -j8             # the Linux frontend
tests/frames/regenerate_reference.sh                   # intentional frame change only
```

Tests do not track header dependencies: run `make -C tests/scripts clean` after header changes.
Never weaken a gate or test.
