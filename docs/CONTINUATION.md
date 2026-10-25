# Phase 2 continuation checkpoint

Last updated: 2026-10-25 (iteration 11: the pickup/chute/death lifecycle slice
is ported — `Kill`'s death-script branch and `Remove`'s live-`Attached` cascade
mirror the C# — and the golden frontier moved from ~t400-900 to the END of the
corpus: L3/L6/L7/L8 match the oracle byte for byte (table AND script) to t30000
with live objects, L4 to t10000 (empty arena), L5 to t2302. The new L5 blocker
is a BOLT spawn at t2303 (leads recorded below). Review-8's depth finding is
recorded as an open item gated on hardware verification.)
This file is the persistent handoff for every fresh context window. Update it at
the end of every batch.

## PRIORITY ORDER (owner directive 2026-10-25 — port code, goldens follow)

The owner's directive: churn through the C#→C++ conversion. Port subsystems
in this order; extend goldens in the same iteration as the code that unlocks
them. Do not spend an iteration on docs/tests alone.

1. **BOLT spawn slice (the new L5 frontier, t2303)** — at t2303 the oracle
   spawns a 16th BOLT in MUSE_1 that the C++ does not spawn (15 vs 16 objects;
   everything matches at t2302). Evidence for the next context: the new BOLT's
   flags are exactly `ProjectileFlags` 0x00080820 (ScriptRuntime.cs:25), while
   the six BOLTs that DO match carry 0x000808A0 = ProjectileFlags |
   FlagNoBanking. C# `Spawn(..., Spawning.Plain)` applies NO static flags
   (cs:1570 — only `Spawning.Flagged` does), and the `{arena}${type}` init
   script runs at creation (cs:1579-1584) — so the NoBanking on the matching
   BOLTs comes from the init script, and the t2303 BOLT's absence means the
   XG's fire DECISION diverged, not the spawn flags: the C++ `fire()`
   (script_runtime.cpp:1531) already matches C# `Fire` (cs:2659) operand for
   operand. Leads, in order: (a) an RNG-gated branch (opcode 47 `if_chance`)
   in the XG attack script — check the C++ `.NET Random` port's draw sequence
   against the oracle's at ~t2303; (b) a visibility/raycast predicate; (c) a
   Kurt-state input. Disassemble the MUSE_1 XG attack script around the
   opcode-61 fire to find the predicate. Then extend L5 past t2302.
2. **Kurt health/inventory state** — the fields opcodes 109/175 read (they
   fire in L3/L5/L6 goldens). Port the state + dump it in the golden table
   so the opcodes become verifiable, then implement them.
3. **Audio slice for opcode 249** — C# `Audio/SoundBank.cs`/`SoundMixer.cs`
   → a C++ sound seam (3DS: no backend yet is fine; the SCRIPT-visible state
   is what the goldens pin). Then implement 249.
4. Continue the remaining 77 opcodes subsystem by subsystem, same rule.

Deferred with evidence: alarm (move_command 15 never fires in the corpus —
verified 0 occurrences in goldens), opcodes whose subsystems are not yet
reachable. Old-3DS hardware timing stays open (hardware-only).

## Current tree state (VERIFIED)

- The kill/remove lifecycle slice is implemented and committed. Verified on the
  final tree:
  - `make -C tests/scripts` → `All cases match the oracle.` — 341 PASS, 0 SKIP,
    including the 36 new deep cases (L3 t480-30000, L4 t660-10000, L5 t720-2302,
    L6 t960-10000, L7 t960-10000, L8 t420-30000; table AND script dumps).
  - `bash tools/check.sh` → `CHECK: ALL GATES PASSED` (`gates_smdh` SKIPs: no
    built `.cia`)
  - `make -C mdk_3ds` (with `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM`)
    → links `mdk.elf`, builds `mdk.3dsx`
  - `cmake --build build-linux --target mdk` → `Built target mdk` — both targets
    build the changed core.
- `docs/OPCODE_COVERAGE.md` was recounted from the brace-matched `Execute`
  spans: C# 238 cases, C++ 161, remaining 77 (the old 155/83 and the old
  "ported" list were wrong — 114, 171, 209 were never in the C++). The
  reachability probe was rerun to t600: of the remaining set only 109 (L3, L5),
  138 (L5), 175 (L6), 249 (L5) fire.

## What this batch did (iteration 11 — pickup/chute/death lifecycle slice)

- **`ScriptRuntime::kill()` now mirrors C# `Kill` (ScriptRuntime.cs:1678)**:
  `health=0`, then either the death-script switch (move_command=0,
  FlagNotTarget, restart/wait_resume = death_script, wait_time=0,
  death_script=0) or the Explode branch. The Explode branch's EFFECTS
  (PlaySoundAt/FlashAt/BreakUp/SpawnExplosion — mixer, flash, debris, explosion
  model) remain unported, but its REMOVAL ACCOUNTING now lands: C# `Explode`
  calls `Remove(obj)` (cs:1717), so the C++ branch calls `remove(obj)` instead
  of the old `dead=true` placeholder (which left the object counted in
  `objectCount()` — found by the adversarial validator). No golden to t30000
  exercises the Explode branch (every corpus death is a death-script death),
  so the missing effects stay the documented gap.
- **`ScriptRuntime::remove()` now mirrors C# `Remove` (cs:1648)**: a live
  `Attached` (a pickup's chute) is removed FIRST (before the parent is marked
  dead), with an `isRemoved()` early-return guard for idempotence (the C++
  `removed_.push_back` is not idempotent, unlike C# `List.Remove`). A pickup
  that falls out (`fallOut` → `remove`) takes its chute with it. The stale
  `fallOut` NOTE (calling kill() a placeholder) was corrected.
- **The frontier moved from ~t400-900 to the end of the corpus.** Probed tick
  by tick against the oracle (table AND script dumps): L3 matches to t30000
  (11 live objects), L8 to t30000 (10), L6 to t30000 (5), L7 to t30000 (7),
  L4 to t10000 (0 objects — the arena empties at the same ticks as the oracle;
  the old "L4 empties past t600" divergence was the kill() placeholder). L5
  matches to t2302; t2303 is the new blocker (BOLT spawn, priority 1 above).
- 36 new golden cases, 72 golden files, all generated from the oracle driver
  (`dotnet mdkoracle.dll <level> <ticks> 1 [script]`). The temp `RALPH_DUMP`
  probe used for the probing was removed from `tick1.cpp` before committing.
- Adversarial validator run over the diff: one MAJOR (Explode-branch count
  accounting) found and fixed as above; the death-script branch field-for-field
  and the Attached cascade order were independently confirmed against the C#.
- Call-site audit vs the C#: C# `Kill` has three call sites (ScriptVm.cs:494/505
  = opcodes 16/56, ported; cs:1554 = opcode 184 `explode`, in the remaining-77
  set, gated behind the unported `Items.Blast`). C# `Remove` call sites map to
  the C++ ones (opcode 110, fallOut, chute-shrink, the Attached cascade); the
  arena-unload removal (cs:677) and the collected-shrink removal (cs:2610)
  belong to unported slices (arena streaming, collected shrink) and do not fire
  in the corpus.

## What this batch did (iteration 8 — deep-tick golden frontier)

- Extended the golden corpus to each level's honest deep-tick frontier, generated
  from the oracle driver (table + script dumps), all matching byte for byte:
  **L3 t360/t420** (pickups/chutes resolve, objects drop 14→12), **L5 t660** (the
  XGEN region past t600), **L6 t660/720/780/840/900** (the XD keeps walking — the
  table genuinely cycles, not a frozen steady state: object 002's pos/yaw/restart
  change across the range), **L7 t660/t900** (a steady arena bracketed to prove no
  slow drift). 10 new cases, 20 new golden files.
- **The frontier is the unported-slice boundary, confirmed empirically, not
  assumed.** Probed deeper ticks against the oracle and committed only the matching
  ones; the first divergent tick per level is the unported lifecycle: **L8 diverges
  at t420** (SW_H01 + SW_CHUTE removed between t360 and t420 — the pickup/chute
  removal slice), **L3 at t480**, **L5 at t720**, and **L4 empties to 0 objects
  past t600** (degenerate, not committed). None of the divergent goldens were
  committed (they would fail).
- **Independently re-verified the testability map's blocker** (so the next context
  does not re-litigate it): `grep` over every golden shows `move=15/` appears **0
  times** — movement command 15 (the only `AlarmTicks` setter, ObjectMotion.cs:385)
  never fires in levels 3-8 to t600, so porting the alarm-ticks state machine +
  dumping `alarm_ticks` would yield an all-zeros column that exercises nothing.
  The four reachable unimplemented opcodes (109/138/175/249) fire but their effects
  are not in the dumped field set (Kurt health, Kurt inventory, audio) or need an
  unported subsystem, so they remain blind-port-forbidden.
- **Corpus cannot widen by level**: only `MDK/TRAVERSE/LEVEL3..8` data is present
  (no level 1/2/9+), so the only axis left to densify is ticks, and that axis is
  now bounded by the unported lifecycle slices above.
- Test-only change; no `mdk_core` code touched, so both targets are unaffected.
  Verified: `make -C tests/scripts` (clean) → `All cases match the oracle.`;
  `bash tools/check.sh` → `CHECK: ALL GATES PASSED` (`gates_smdh` SKIPs: no `.cia`);
  `make -C mdk_3ds` → links `mdk.3dsx`; `cmake --build build-linux --target mdk` →
  `Built target mdk`.

## What this batch did (iteration 7 — target-tracking goldens)

- Executed the testability map's recommended unblock (the "extend BOTH the oracle
  driver's `script` dump and `tick1.cpp` `dumpScript`" move), scoped to the
  runtime fields the C++ **already** maintains so the change is a pure
  regression-lock with no behaviour change: `target_pos`, `target_yaw`,
  `alien_target`. These are `ScriptRuntime`-level state (C# `TargetPosition`/
  `TargetYaw`/`AlienTarget`, ScriptRuntime.cs:199-208), reset to Kurt each tick
  (cs:887-889) and overridable per object by `SelectTarget` (cs:418-441); the C++
  mirrors this in `selectTarget` (script_runtime.cpp:2192) and the constructor.
- **Measured before committing**: extended both dumps, ran the C++ against the
  OLD goldens (117 cases differ — the SCRIPT line changed), then confirmed the
  C++ `got` SCRIPT lines equal the oracle's for L3_t300/L5_t600/L8_t300/L4_t600
  byte for byte. Regenerated all 114 script goldens from the oracle → all match.
- **Sensitivity proven**: a `+0.0001f` perturbation of the dumped `target_yaw`
  makes the script checks FAIL (then reverted) — the new fields are genuinely
  compared, not ignored.
- `alien_target` is `-1` in every current case (opcode 251 does not fire in
  levels 3-8 to t600), so the index mapping is correct-but-unexercised; it is
  computed over the same removed-skipped enumeration as the OBJ lines, so the
  two views agree when it does fire.
- **`alarm_ticks` deliberately NOT added**: the C++ has no `alarm_ticks` field
  (the alarm-ticks state machine — the per-tick decrement cs:896 + the setters in
  movement command 15 / Rides — is unported), so dumping it would diverge and
  force a blind port. It stays the next field to add, paired with the alarm
  subsystem slice. `Items.Bomb`/`Decoy` also stay out: they live on a separate
  `Items` subsystem not reachable from the driver, and the C++ `itemsBomb` is
  always null (matches the C# when no bomb is in play).
- **Oracle-driver provenance** (the driver is `/tmp/kilo/oracle/Driver/Program.cs`,
  not in the repo — same as every existing golden): the `script` dump mode's
  SCRIPT line gained `target_pos={X:F4},{Y:F4},{Z:F4} target_yaw={Yaw:F4}
  alien_target={idx}`, where `idx` is `AlienTarget`'s index in `scripts.Objects`
  or `-1`. Regenerate with
  `dotnet mdkoracle.dll <level> <ticks> 1 script > golden/L<level>_t<ticks>_script.txt`.
- Verified: `make -C tests/scripts` (clean) → `All cases match the oracle.`;
  `bash tools/check.sh` → `CHECK: ALL GATES PASSED` (`gates_smdh` SKIPs: no
  `.cia`); `make -C mdk_3ds` → links `mdk.3dsx`; `cmake --build build-linux
  --target mdk` → links `mdk`. Test-only + oracle-harness change; no `mdk_core`
  code touched, so both targets are unaffected.

## What this batch did (iteration 6 — verification, review, doc correction)

- **Tree re-verified green** on a clean rebuild (iteration 5 timed out mid-final-
  verification after committing the scene code `003f9a3` but before committing its
  `CONTINUATION.md` update):
  - `make -C tests/scripts` (after `clean`) → `All cases match the oracle.` (all
    table + script + `L{n}_dti` + the three scene checks PASS).
  - `bash tools/check.sh` → `CHECK: ALL GATES PASSED` (`gates_smdh` SKIPs: no built
    `.cia`).
  - `make -C mdk_3ds` → stages RomFS (69 files, 79M) and links `mdk.3dsx`.
- Committed iteration 5's dangling `CONTINUATION.md` update (the dynamic-scene
  batch note) as its own commit — the code was already committed, only the doc was
  left in the working tree.
- **Adversarially reviewed the scene commit `003f9a3`** against the C# `LevelView`
  (the four external reviews all timed out at rc=124 mid-verification and produced
  no findings). Checked: the run-break at group boundaries (a batch is single-group,
  matching the C#'s per-group meshes batched by surface); the orphaned-vertex path
  when a not-drawn triangle splits a same-material/same-group run (the split triangle's
  3 vertices are pushed but referenced by no batch — wasted buffer, never drawn, and
  the neighbouring batches' `first`/`count` stay correct); `arenaBatchVisible`
  (group 0 has no stored state → always drawn); `LevelView::refresh()` (rebuilds the
  whole arena mesh for a `group_set_texture`, the C# per-group `Rebuild` produces the
  same picture; the texture/flat-colour caches grow monotonically, matching the C#
  "old mesh lives until the level ends"). **No bug found.**
- **Corrected a real mis-classification** in `docs/OPCODE_COVERAGE.md`: opcode **27**
  was grouped under "Audio (no mixer port)". It is `if_alarm`, a branch on
  `runtime.AlarmTicks > 0` (ScriptVm.cs:1693) — gated behind the **alarm-ticks state
  machine** (a runtime `alarm_ticks` field + the per-tick decrement
  ScriptRuntime.cs:896 + the setters in movement command 15 ObjectMotion.cs:385 and
  Rides Rides.cs:219), none of which exists in the C++. 107/233/249 are the real
  audio opcodes. 27 does not fire in levels 3-8 to t600, so there is no golden to
  exercise it — it stays deferred, now with the correct gate named.

## Testability map (why the remaining opcode work is blocked, not merely deferred)

The goldens compare a FIXED field set (`tick1.cpp` `dumpTable`/`dumpScript`):
pos, yaw, hp, flags, dead, move_command/parameter, connects, path/time/speed/stop,
script_flags, variables, restart, wait, death_script, command_target, gosub stacks,
level_timers, global vars. They do **not** observe: `target_position`/`target_yaw`,
`alien_target`, `alarm_ticks`, `Items.Bomb`/`Items.Decoy`, velocity, or any audio
state. Combined with the reachability probe (only 109/138/175/249 fire among the
remaining 77), every remaining owned-partial or unimplemented opcode falls in one of
three buckets, none of which is testable in the current tree:

1. **Gated behind an unported subsystem** (the subsystem must land first): 109
   (Rides + `Kurt.Hurt`), 138 (`Debris.Shatter`), 175 (`Kurt.Inventory`), 249/107/
   233 (audio `Mixer`), 27 (alarm-ticks state machine), 165/166 (`Items.Bomb`),
   decoy targeting (`Items.Decoy`), 16/56 death path (death script/debris/drops).
2. **Fires but its effect is not in the dumped field set** — e.g. movement command 6
   (`Chase`) and 15 (`AlarmTicks`): if 6 fired and moved an object the goldens would
   already FAIL, so it does not fire in levels 3-8/t600; 15's only observable effect
   (`alarm_ticks`) is not dumped. Implementing them cannot be checked.
3. **In the dumped set and already matches** — the goldens pass byte-for-byte, so any
   owned opcode that fires and writes a dumped field is already verified.

The project rule (OPCODE_COVERAGE.md) forbids porting an opcode without a test that
exercises it, so buckets 1 and 2 must NOT be blind-ported. **To unblock the next
tier**, the highest-leverage move is to extend BOTH the oracle driver's `script`
dump (`/tmp/kilo/oracle/Driver/Program.cs`) and `tick1.cpp` `dumpScript` to observe
`alarm_ticks`, `target_position`/`target_yaw`, `alien_target`, and `Items.Bomb`/
`Decoy`, then regenerate the script goldens — that turns bucket 2 into a real test
and makes the Items/alarm subsystem slices verifiable when they land. That is a
deliberate golden-set change (touches the oracle harness and regenerates every
script golden), so it is its own increment, not a drive-by.

**Progress (iteration 7):** `target_position`/`target_yaw`/`alien_target` are DONE —
dumped and locked into all 114 script goldens (the C++ already matched, so no
behaviour change). The remaining dump fields are each gated behind an unported
subsystem and must be added *with* that subsystem, not before it: `alarm_ticks`
with the alarm-ticks state machine (the per-tick decrement + the movement-command-15
/ Rides setters), and `Items.Bomb`/`Decoy` with the `Items` slice (which must also
become reachable from the oracle driver). Adding either field to the dump before its
subsystem lands would make the goldens diverge and force a blind port.

**Progress (iteration 8):** the alarm-ticks unblock is now shown to be a dead end in
*this* corpus, not merely deferred: `move=15/` (the only `AlarmTicks` setter) appears
**0 times** across every golden, so dumping `alarm_ticks` would lock an all-zeros
column. The deep-tick frontier was extended instead (see the iteration 8 batch note):
the corpus now reaches each level's first unported-lifecycle divergence (L8 t420,
L3 t480, L5 t720; L4 empties past t600). The next real unblock is a subsystem whose
setter actually fires in levels 3-8 — the pickup/chute/death lifecycle is the first
one on that frontier (it is what bounds L8 at t420), so porting it (with a dump field
for the removal) is the highest-leverage next slice, not the alarm.

## What this batch did (dynamic scene updates — open item 1)

- Open item 1 ("a mid-play `group_state`/`group_set_texture` change does not
  change what is drawn until the scene is rebuilt") is **closed at the scene
  level**, faithful to the C# `LevelView`, which checks Hidden at DRAW time
  (LevelView.cs:126) and rebuilds a group's mesh on a material change (the
  `Changed` event → `Rebuild`, LevelView.cs:251).
- `ArenaBatch` gained a `group` field, and `buildArenaScene` now breaks a run at
  a **group boundary** as well as a material change, so every batch is single-
  group. The build-time Hidden skip is **gone**: hidden groups' geometry stays in
  the mesh and `arenaBatchVisible(arena, batch)` (a new free function) decides
  per batch at draw time — so `group_set_state` hide/show needs **no rebuild**.
  `LevelView::draw()` skips a batch whose group is Hidden now.
- `LevelView::refresh()` is the `group_set_texture` path (the C# `Rebuild`): it
  re-runs `buildArenaScene` against the arena's current group materials and
  rebuilds the GPU mesh (release + create, like the C# creating a new mesh). It
  is the integration hook a `group_set_texture` calls between frames; there is no
  `GameEngine` yet to call it, so the mechanism is in place and tested at the
  scene level, and the per-opcode wiring lands with the engine. A hide/show does
  NOT call it (draw reads hidden live).
- **Regression test** (`tests/scripts/tick1.cpp`, scene section): over **all 104
  arenas** of levels 3-8 it checks (a) the batches partition the drawn triangles
  by group — a batch spanning two groups breaks this equality, so this proves the
  run-break; **880 cross-group same-material adjacencies** exercise it (removing
  the run-break makes it FAIL on HMO_2/HMO_3/MEAT_*/…, confirmed); (b) hiding a
  group after the build makes its batches not-visible while others stay visible;
  (c) `group_set_texture(-1)` on a textured group makes its batches flat on a
  rebuild. `arena_scene.cpp` + `material_resolver.cpp` added to the test Makefile.
- Verified: `make -C tests/scripts` (clean) → `All cases match the oracle.` (new
  scene PASSes); `bash tools/check.sh` → `CHECK: ALL GATES PASSED`; `make -C
  mdk_3ds` → links `mdk.3dsx`; `cmake --build build-linux --target mdk` → links
  `mdk` (both targets build the changed core).

## What this batch did (DTI record discrepancy — reproduced and closed)

- Open item 4's "C++/oracle DTI record discrepancy" is **CLOSED with a concrete
  reproduction**, not an argument (the prior batch had only argued the layouts
  match field-for-field).
- Reproduction: dumped every arena's object records for levels 3-8 from BOTH the
  C# oracle (`Dti.ParseRecords`) and the C++ `Dti::parseRecords`, printing every
  float (angle, position[3], boxEnd[3], arena pitch) as **raw IEEE bits** so a
  one-ULP difference cannot hide behind `%.4f`. Result: **byte-identical across
  all 6 levels, 19 arenas each, every record** — type/id/angle/position/boxEnd
  (the three-floats-then-`skip(-12)`-then-name(12) rewind quirk) and name. The
  claim was unsubstantiated; there is no DTI record discrepancy.
- Locked it with a **permanent regression test**: `tests/scripts/tick1.cpp` now
  dumps the same raw-bit record view and compares to committed goldens
  `tests/scripts/golden/L{3..8}_dti.txt` (generated from the oracle). The test is
  sensitive — a probe aliasing bug during authoring made the C++ dump differ, and
  the check caught it, confirming it fails on a real divergence.
- Reproduce/regenerate: the oracle driver gained a `dti` dump mode
  (`/tmp/kilo/oracle/Driver/Program.cs`, 4th arg `dti`) that prints the records
  in this format; the goldens are that output. (The driver is the oracle harness,
  not in the repo — same as the existing goldens' provenance.)
- Test-only change; no `mdk_core` code touched, so both targets are unaffected.
  Verified: `make -C tests/scripts` (clean) → `All cases match the oracle.` (six
  `L{n}_dti` PASS); `bash tools/check.sh` → `CHECK: ALL GATES PASSED`;
  `make -C mdk_3ds` → links `mdk.3dsx`.

## What this batch did (owned-opcode classification — Task 2 remainder)

- Open item 2 (classify the 161 owned cases) is **done**. The classification is
  in `docs/OPCODE_COVERAGE.md` §"Owned-case classification", produced by a
  reproducible brace-matched extraction of the numeric `case` labels from the C#
  `ScriptVm.Execute` and the C++ `ScriptVm::execute` (re-confirmed C# 238 / C++
  161 / remaining 77; the C++ owns no case the C# lacks).
- Result: **0 stubs**, **11 partial** (faithful dispatch whose gameplay effect
  is gated behind a partial/unported subsystem), **150 complete**. Each partial
  is verified against the code, not asserted:
  - 6, 15 — set `move_command` 6/15 but `ScriptRuntime::updateCommand`
    (script_runtime.cpp:1561) has no case for them → no motion.
  - 16, 56 — call `kill()`, a death-slice placeholder (script_runtime.cpp:2451
    sets only `dead`; death script / debris / drops unported).
  - 24, 151 — store sound names whose playback needs the unported mixer.
  - 89, 248 — honest no-ops (mixer / Kurt knock-back unported; no state the
    goldens read).
  - 140 — sets group material VM state; renderer ignores it (open item 1).
  - 165, 166 — read `itemsBomb`, never assigned (script_runtime.h:226) → always
    "no bomb" until the Items slice.
- Test-coverage axis recorded separately: all 161 owned cases are covered only by
  the end-to-end (level, tick) goldens; there are no per-opcode unit tests.
- Doc-only change; no `mdk_core` code touched, so both targets are unaffected.
  Verified: `make -C tests/scripts` → `All cases match the oracle.`;
  `bash tools/check.sh` → `CHECK: ALL GATES PASSED`; `make -C mdk_3ds` → links
  `mdk.3dsx`.

## What the prior batch did (L5 deep-tick goldens — divergence closed)

- The open "L5 `XG[004]` golden divergence ~tick 577 (root cause unknown)" is
  **resolved**. Reproduced against the oracle: at t600 the oracle's object count
  oscillates 8→9→8 (the XGEN spawner's 5th XG opens ~t585 and dies ~t595) and
  object 004 takes a `move_near_target` (opcode 43) at ~t577. The current C++
  matches the oracle **byte for byte** (table AND script state) at t577/585/590/
  595/600. The old divergence was the same degree/radian constant-fold ULP class
  the left-associative spelling already resolves (the stale `/tmp/kilo/cpp_L5_t600_*.txt`
  dumps predated that fix and misled the earlier note).
- Added golden cases `{5,577},{5,585},{5,590},{5,595},{5,600}` (table + script)
  to `tests/scripts/tick1.cpp`, generated from the oracle. Verified:
  - `make -C tests/scripts` → `All cases match the oracle.` (new cases PASS,
    table + script).
  - `bash tools/check.sh` → `CHECK: ALL GATES PASSED`.
  - `make -C mdk_3ds` → links `mdk.3dsx`.
  - Test-only change; no `mdk_core` code touched, so both targets are unaffected.

## What the previous batch did (all verified against the C# oracle)

- `TriangleGroups.SetState` semantics: stored per-group state on `Arena`
  (`groupState`/`setGroupState`/`hasGroup`), full 1..255 group numbers, the
  0/2/3/4/5/other operation table, write-through to every triangle of the
  group. `ScriptRuntime::setGroupState` mirrors it; the old per-triangle
  derivation and the ≤16 clamp are gone.
- Load-time `Disable`: `Arena::applyDisabledTriangles()` runs in the
  `ScriptRuntime` constructor (after clip flags), not in `Arena::parse` —
  LEVEL8 GUNT_2's diagonal wall is now non-solid like the original.
- `raycastGroup` mirrors `raycastPoint` exactly (`kSegmentAny`,
  `kClipPassesThrough`, second arena unconditional, all-arena fallback,
  nearest-hit by squared distance, same `>= best` skip);
  `getKurtFloorGroup` uses it (+1 to −10 under Kurt).
- `HitGroup` implemented, including the scratch-object run path: the arena's
  `hit_scripts` `MdkObject` gets hit_type=weapon, hit_event=0, wait_time=0,
  cleared gosub stacks, restart=script, then `vm_.run()`.
- Group materials: `Arena::groupMaterial(group, int32_t* out)` returns
  presence (negative materials like −1 are valid; the old −1 sentinel was a
  bug the validator caught). `setGroupTexture` (opcode 140) wired with the
  existence check.
- Renderer build-time skips: clip/disabled triangles and hidden groups.
- Opcodes audited per Task 1.7: 245 `find_object` (factored into a runtime
  helper), 246, 250 `if_in_box` (six-value zero-filled array + sender), 87
  `kurtLooksAt` (constants match ScriptRuntime.cs:2701), 6/15, 62, 207.
- VM group cases 98/99/140/162/163/168/194 match C# operand layout.

## Open items (do not lose these)

1. **Dynamic scene updates — DONE at the scene level.** `buildArenaScene` tags
   each batch with its group and breaks runs at group boundaries; Hidden is a
   draw-time check (`arenaBatchVisible`), so `group_set_state` hide/show needs no
   rebuild, and `LevelView::refresh()` rebuilds the scene+mesh for a
   `group_set_texture` (the C# `Rebuild`). Locked by the scene test in
   `tests/scripts/tick1.cpp`. **Remaining:** the `GameEngine` that runs the
   scripts must call `LevelView::refresh()` when a `group_set_texture` fires (no
   engine exists yet); the VM state and the scene mechanism are both correct and
   tested, only the per-opcode call-site wiring is pending. Do not fake it.
2. **Opcode classification (Task 2 remainder) — DONE.** The 161 owned cases are
   classified in `docs/OPCODE_COVERAGE.md` §"Owned-case classification": 0 stubs,
   11 partial (each verified against the gating subsystem), 150 complete. Per-opcode
   unit tests beyond the end-to-end goldens are still needed (the goldens are the
   only coverage today).
3. **Reachable unimplemented opcodes**: 109, 138, 175, 249 fire in levels 3-8
   to t600 and are real gaps. Each is gated behind an unported subsystem
   (109 → Rides + `Kurt.Hurt`; 138 → `Debris.Shatter`; 175 → `Kurt.Inventory`;
   249 → the audio `Mixer`), so none is meaningfully testable until that
   subsystem lands — do not blind-port them (see coverage doc).
4. **Known divergences**: the former "C++/oracle DTI record discrepancy" is
   **CLOSED** — reproduced with a raw-IEEE-bit dump of every arena's records for
   levels 3-8 from both parsers, byte-identical (see the batch note above), and
   locked by `tests/scripts/golden/L{3..8}_dti.txt`. Remaining known divergences:
   incomplete decoy/`AlienTarget` targeting; opcode 207 — C# `MathF.Sign` throws
   on NaN (C++ returns 0; exceptions are banned here, accepted divergence). (The
   former "L5 `XG[004]` divergence ~t577" is CLOSED — see the batch note above.)
5. **Old 3DS hardware timing: UNMEASURED.** A devkitARM `.3dsx` is not
   hardware validation. Never claim an OG pass without a capture.
6. **C3D depth state diverges from the oracle (review-8 finding, verified).**
   The C# renderer depth-tests and depth-writes mesh geometry in the original
   look: `enable_depth_test = geometry == Mesh && pass not Overlay/OnTop`,
   `enable_depth_write = Mesh && !blended && pass != OnTop`, compare
   LESS_OR_EQUAL (`src/Mdk.Engine/Render/Renderer.cs:603-608` — note the
   renderer oracle lives in `src/Mdk.Engine`, not `src/Mdk.Game`). The C3D
   backend disables the depth test outright (`mdk_3ds/src/render/renderer_c3d.cpp:624`,
   `C3D_DepthTest(false, GPU_ALWAYS, 0)`), and its comment's claim that "the
   C#'s original-look path" draws back-to-front without depth is REFUTED by
   the oracle above. The fix is NOT a one-line flip: PICA's depth range is
   [-1,0] (opposite of D3D's [0,1]), the comment documents an observed
   invisible-geometry trap from a desktop-style clear, and AGENTS.md P6
   requires a hardware/emulator capture for render-path changes (no emulator
   is available in this environment). The next render pass must map the C#
   LESS_OR_EQUAL semantics onto PICA (projection remap at renderer_c3d.cpp:496
   + `C3D_DepthMap` + test function + clear value) and verify on hardware or
   a working emulator before claiming it.
7. **DepthLayer/DepthPull unported (review-8 finding).** The C# pairs
   `Layers.Of` (`src/Mdk.Game/Level/Layers.cs`) with `DepthPull.cs` to push
   per-triangle depth layers (a poster over its wall); `render_types.h`
   documents `DepthPull`/`depth_layer` but no C++ code computes or applies
   them. Gated on item 6 (they are depth-buffer semantics) and on the textured
   renderer path.
8. **Doc/spec drift (review-8 observations, not code).** `AGENTS.md`/`SPEC.md`
   cite `docs/DOCTRINE.md`, which does not exist; SPEC §9.4 lists a
   `gates_romfs` gate that was never written. Both are documentation/spec
   corrections, not porting work — flag to the owner rather than inventing a
   gate or doctrine text unilaterally.

## Verification commands (run after every batch)

```
make -C tests/scripts        # builds + goldens ("All cases match the oracle")
bash tools/check.sh          # the six contract gates
make -C mdk_3ds              # devkitARM target (mdk.3dsx)
```

The tests Makefile has no header dependency tracking — after editing headers,
`make -C tests/scripts clean` first, or stale objects can link against old
signatures.

Rules: `src/Mdk.Game/**` is read-only oracle. Keep the monolithic
`ScriptVm::execute()` switch (dispatch investigation resolved). Never weaken
a gate or test to pass. Commit per verified batch.
