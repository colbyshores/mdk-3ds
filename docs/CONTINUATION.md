# Phase 2 continuation checkpoint

Last updated: 2026-10-25 (dynamic scene updates: mid-play group hide is a
draw-time check, group retexture rebuilds the scene; scene regression test added).
This file is the persistent handoff for every fresh context window. Update it at
the end of every batch.

## Current tree state (VERIFIED)

- The group-state slice (Task 1 below) is implemented and committed. Clean
  rebuild verified:
  - `make -C tests/scripts` → `All cases match the oracle.` (after `clean`), now
    including the six `L{n}_dti` record-layout checks.
  - `bash tools/check.sh` → `CHECK: ALL GATES PASSED` (`gates_smdh` SKIPs: no
    built `.cia`)
  - `make -C mdk_3ds` → links `mdk.elf`, builds `mdk.3dsx`
  - `cmake --build build-linux` (the `mdk` target) also compiles and links —
    both targets build the changed core.
- `docs/OPCODE_COVERAGE.md` was recounted from the brace-matched `Execute`
  spans: C# 238 cases, C++ 161, remaining 77 (the old 155/83 and the old
  "ported" list were wrong — 114, 171, 209 were never in the C++). The
  reachability probe was rerun to t600: of the remaining set only 109 (L3, L5),
  138 (L5), 175 (L6), 249 (L5) fire.

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
