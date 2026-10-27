# Phase 2 continuation checkpoint

Last updated: 2026-10-27 (iteration 50: opcode 217 signed-add and percent truncation tests).
Current coverage: 190 true C++ dispatches / 238, 48 remaining. Continue one
opcode family per batch via `vm_tests`; renderer work waits for P5.0.

This is persistent handoff. Keep it under ~150 lines; batch narrative goes in
`docs/PROGRESS_LOG.md`.

## PRIORITY ORDER (owner directive 2026-10-25)

1. **DONE — mixer call-sites** (`ObjectMotion.cs:133`, `ObjectBehaviors.cs:90-115`).
2. **DONE — `Debris.Shatter` + opcodes 137/138/139**, dump line and oracle
   goldens. BreakUp/Trail/Updraft await Effects.
3. Continue opcode conversion subsystem by subsystem; tests/goldens in the same
   batch. Opcode 208 (reachability probe), harness + 13, parts 32/129,
   rope-swing 226/242, spawn 156/206, push-hit-dir 81, stats 217, vision 243,
   value slots 199/210, Kurt/screen 215/240/244, and no-ops 239/77 are
   implemented and pinned. Newer slices include 85 (`set_rolling`) and 74
   (`attach_to`). 132/136 still need Effects; frame-blend integration remains a
   separate future slice. Remaining 48 are listed in `docs/OPCODE_COVERAGE.md`.
4. **P5.0 — frame-golden harness** (`SPEC-COMPLETION.md` §3 item 4) before
   renderer batches. Owner must select headless C# vs original-EXE reference;
   no renderer batch until the harness gate is green.

Deferred: alarm/move 15 fires zero times in current corpus; 248 `push_kurt` is
an honest no-op pending Kurt knock-back. Old-3DS timing remains UNMEASURED.

## Current tree state (VERIFIED)

- Iteration 50 strengthens opcode 217's `GameStats` test: a negative S32
  operand subtracts from `headShots`, disabled counting remains unchanged, and
  `Percent(2, 3)` pins integer truncation to 66. `make -C tests/scripts clean &&
  make -C tests/scripts` passed 23 VM tests and the full oracle suite; `bash
  tools/check.sh` passed all gates (SMDH skipped, no CIA); the 3DS build staged
  69 RomFS files (79M) and produced `mdk.3dsx`. No golden changed; no
  emulator/hardware run. Old-3DS timing remains unmeasured.
- Iteration 51 closes review-10's `attach_to` fixture gap by adding a live
  different-type same-arena candidate with colliding instance ID 1000. This
  rejects ID-only matching. `docs/OPCODE_COVERAGE.md` records all pinned
  TYPE_ID discrimination cases. `make -C tests/scripts clean && make -C
  tests/scripts` passed 23 VM tests and the full oracle suite (`All cases match
  the oracle.`); `bash tools/check.sh` passed all gates (SMDH skipped, no CIA);
  the 3DS build staged 69 RomFS files (79M) and produced `mdk.3dsx`. No golden
  changed, no emulator/hardware run. Old-3DS timing remains unmeasured.
- Next: continue opcode conversion from the remaining 48 in
  `docs/OPCODE_COVERAGE.md`; select the next testable subsystem slice.
- Iteration 49 addresses review-9: the attach test adds same-arena `BEAMS_999`
  and `BEAMS_10001` before self/dead/cross-arena exclusions. Thus lower-bound/
  range and canonical-name-prefix resolvers cannot pass the exact-ID checks,
  while exclusion assertions remain independently sensitive to scan order.
  `make -C tests/scripts clean && make -C tests/scripts` passed all 23 VM tests
  and the full oracle suite (`All cases match the oracle.`). `bash tools/check.sh`
  reported all gates passed; SMDH was skipped because no CIA exists. The 3DS
  build staged 69 RomFS files (79M) and produced `mdk.3dsx`. `git diff --check`
  passed. No golden changed; no emulator/hardware run. Old-3DS timing remains
  unmeasured.
- Iteration 48 added a same-arena `BEAMS_1001` candidate, requiring requests
  for `BEAMS_1000` and `BEAMS_1001` to select the matching IDs. Iteration 47
  made self/dead/cross-arena exclusions independently testable. Iteration 46
  ported opcode 74 with C# auto IDs starting at 1000, duplicate-ID last-match
  selection, case folding, exclusions, and miss-state preservation. Details
  and verification for iterations 44–50 are in `docs/PROGRESS_LOG.md`.

## Testability and standing open items

- Oracle/goldens compare fixed table + script fields, not velocity,
  `alarm_ticks`, `Items.Bomb`/`Decoy`, or all combat/effect state. Use `vm_tests`
  for otherwise unreachable opcode behavior; don't port without a test. The
  tick corpus matches byte-for-byte through t30000 on levels 3–8.
- Oracle driver: `/tmp/kilo/oracle/Driver/Program.cs`; run with
  `DOTNET_ROOT=/tmp/kilo/dotnet10`, `MDK_DATA_DIR=<repo>/MDK`, driver args
  `L T 1 script`. Source oracle `src/Mdk.Game/**` is read-only.
- Dynamic scene refresh exists at scene level; no GameEngine exists to wire
  refresh on opcode 140. Do not fake call-site wiring.
- Known divergences: incomplete decoy/`AlienTarget` target selection; opcode
  207's `MathF.Sign(NaN)` exception is accepted because C++ exceptions banned;
  degenerate-path guards return 0 rather than C# out-of-range behavior.
- C3D depth state still differs from C#: depth test disabled in C3D versus
  LESS_OR_EQUAL depth test/write in C#. Resolution needs projection remap,
  `C3D_DepthMap`, test function and clear value plus capture. DepthLayer/
  DepthPull are unported and depend on textured renderer path.
- Old 3DS hardware timing UNMEASURED. Never claim OG readiness without actual
  Old 3DS capture. Emulator GUI runs must use explicit `DISPLAY=:99` and save
  captures under `/tmp/kilo`.
- Documentation drift noted earlier: `AGENTS.md`/`SPEC.md` cite absent
  `docs/DOCTRINE.md`; SPEC §9.4 names an unwritten `gates_romfs`. Flag to owner,
  do not invent policy/gates unilaterally.

## Verification commands

```
make -C tests/scripts
bash tools/check.sh
DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds
```

Tests do not track header dependencies: run `make -C tests/scripts clean` after
header changes. Keep the monolithic `ScriptVm::execute()` switch. Never weaken a
gate or test.
