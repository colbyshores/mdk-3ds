# Phase 2 continuation checkpoint

Last updated: 2026-10-28 (iteration 68: opcode 135 screen-shake state).
Current coverage: 205 true C++ dispatches / 238, 33 remaining. Continue one
opcode family per batch via `vm_tests`; renderer work waits for P5.0.

This is persistent handoff. Keep it under ~150 lines; batch narrative goes in
`docs/PROGRESS_LOG.md`.

## PRIORITY ORDER (owner directive 2026-10-25)

1. **DONE — mixer call-sites** (`ObjectMotion.cs:133`, `ObjectBehaviors.cs:90-115`).
2. **DONE — `Debris.Shatter` + opcodes 137/138/139**, dump line and oracle
   goldens. BreakUp/Trail/Updraft await Effects/Fans.
3. Continue opcode conversion subsystem by subsystem; tests/goldens in the same
   batch. Opcode 208 (reachability probe), harness + 13, parts 32/129,
   rope-swing 226/242, spawn 156/206, push-hit-dir 81, stats 217, vision 243,
   value slots 199/210, Kurt/screen 215/240/244, no-ops 239/77, object avoidance
   204, turn-and-jump 229, floor-below 121, no-flat-floor 192, hit-weapon
   195 and screen-shake 135 are implemented and pinned. The current-plus-second
   raycast selection seam is pinned against overlapping DANT_1/DANT_8 geometry.
   All three oracle `Linked` writers (245/179/228) are ported, so
   partner pairs can feed the 213/214 conditions. 132/136 still need Effects;
   frame-blend integration is a separate future slice. The exact remaining 33
   are in
   `docs/OPCODE_COVERAGE.md`.
4. **P5.0 — frame-golden harness** (`SPEC-COMPLETION.md` §3 item 4) before
   renderer batches. Owner must select headless C# vs original-EXE reference;
   no renderer batch until the harness gate is green.

Deferred: 248 `push_kurt` is an honest no-op pending Kurt knock-back. The
C#'s Rides subsystem also writes the alarm countdown and remains deferred with
Rides. Opcode 135's follow-camera consumer, opcode 195's group-hit/direct
combat writers, and opcode 81's hit-direction writers remain partial with
those subsystems. Old-3DS timing remains UNMEASURED.

## Current tree state (VERIFIED)

- Iteration 68 ports opcode 135 (`raise_573aa8`) from
  `ScriptVm.cs:1581-1582` and `ScriptRuntime.cs:899,1986-1989`. The runtime
  clears `shake` at tick start and keeps the strongest request, including the
  oracle's NaN-propagating `MathF.Max` behavior. One synthetic `vm_tests` case
  pins the dispatch, maximum sequence, negative request, NaN and next-tick
  reset. The C# follow-camera consumer remains a separate renderer slice.
- Review-11's three MINOR findings are addressed: opcode 195's test now pins
  signed equality with two sign-crossing cases (an absolute-value mutant now
  exits 1 with two failures); the deferral note now says the ported
  `ScriptRuntime::hitGroup` is uncalled and names the C# group-hit call sites,
  including Kurt's `HitTypeKurt = -11` touch; and the ledger applies one
  Complete/Partial rule (unwritten input or missing consumer means partial).
  That rule puts 81/135/195 in the 13-case partial table, leaving 192 complete.
- Review-11 found no BLOCKER or MAJOR. Its PASS-print NIT is pre-existing; the
  verification below cites exit codes. Its DANT_8 exact-coordinate observation
  remains unproven and was not changed in this opcode slice.
- Coverage is now 205 true dispatches / 238 with 33 remaining. The
  brace-matched recount gives C# 238 cases and C++ 207 numeric labels / 206
  unique labels; the nested opcode-249 switch accounts for the extra label and
  the duplicate.
- Verification on this tree:
  - `make -C tests/scripts clean && make -C tests/scripts` exited 0 after 40 VM
    tests and the complete oracle suite (`All cases match the oracle.`).
  - `bash tools/check.sh` exited 0 with all contract gates (`CHECK: ALL GATES
    PASSED`); SMDH was skipped because no CIA was built.
  - `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds` exited 0, linked `mdk.elf`, staged 69 RomFS files (79M), and built
    `mdk.3dsx`.
  - `cmake --build build-linux --target mdk` exited 0 and built the Linux target.
  - `MDK_DATA_DIR=<repo>/MDK tests/scripts/build/tick1 probe 30000` exited 0
    and reported no unimplemented opcodes on levels 3–8.
- No golden changed. No emulator or hardware run was performed. Old-3DS timing
  remains unmeasured.
- Next: port opcode 170 (`inherit_kurt_vel`) with a runtime Kurt-velocity seam
  and a synthetic test. Opcode 28 remains with the SniperRounds subsystem; do
  not blind-port either without the state they consume.

## Testability and standing open items

- Oracle/goldens compare fixed table + script fields, not velocity,
  `alarm_ticks`, `Items.Bomb`/`Decoy`, or all combat/effect state. Use
  `vm_tests` for otherwise unreachable opcode behavior; don't port without a
  test. The tick corpus matches byte-for-byte through t30000 on levels 3–8.
- Oracle driver: `/tmp/kilo/oracle/Driver/Program.cs`; run with
  `DOTNET_ROOT=/tmp/kilo/dotnet10`, `MDK_DATA_DIR=<repo>/MDK`, driver args
  `L T 1 script`. Source oracle `src/Mdk.Game/**` is read-only.
- Dynamic scene refresh exists at scene level; no GameEngine exists to wire
  refresh on opcode 140. Do not fake call-site wiring.
- Known divergences: incomplete decoy/`AlienTarget` target selection; opcode
  207's `MathF.Sign(NaN)` exception is accepted because C++ exceptions are
  banned; degenerate-path guards return 0 rather than C# out-of-range behavior.
- C3D depth state still differs from C#: depth test disabled in C3D versus
  LESS_OR_EQUAL depth test/write in C#. Resolution needs projection remap,
  `C3D_DepthMap`, test function and clear value plus capture. DepthLayer/
  DepthPull are unported and depend on the textured renderer path.
- Old 3DS hardware timing UNMEASURED. Never claim OG readiness without actual
  Old 3DS capture. Emulator GUI runs must use explicit `DISPLAY=:99` and save
  captures under `/tmp/kilo`.
- Documentation drift noted earlier: `AGENTS.md`/`SPEC.md` cite absent
  `docs/DOCTRINE.md`; SPEC §9.4 names an unwritten `gates_romfs`. Flag to owner,
  do not invent policy/gates unilaterally.

## Verification commands

```
make -C tests/scripts clean && make -C tests/scripts
bash tools/check.sh
DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds
```

Tests do not track header dependencies: run `make -C tests/scripts clean` after
header changes. Keep the monolithic `ScriptVm::execute()` switch. Never weaken
a gate or test.
