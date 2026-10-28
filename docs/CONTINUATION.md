# Phase 2 continuation checkpoint

Last updated: 2026-10-28 (opcode 220 place-near-Kurt batch).
Current coverage: 209 true C++ dispatches / 238, 29 remaining. Continue one
opcode family per batch via `vm_tests`; renderer work waits for P5.0.

This is persistent handoff. Keep it under ~150 lines; batch narrative goes in
`docs/PROGRESS_LOG.md`.

## PRIORITY ORDER (owner directive 2026-10-25)

1. **DONE — mixer call-sites** (`ObjectMotion.cs:133`, `ObjectBehaviors.cs:90-115`).
2. **DONE — `Debris.Shatter` + opcodes 137/138/139**, dump line and oracle
   goldens. BreakUp/Trail/Updraft await Effects/Fans.
3. Continue opcode conversion subsystem by subsystem; tests/goldens in the same
   batch. Opcodes 208 (reachability probe), harness + 13, parts 32/129,
   rope-swing 226/242, spawn 156/206, push-hit-dir 81, stats 217, vision 243,
   value slots 199/210, Kurt/screen 215/240/244, no-ops 239/77, object
   avoidance 204, turn-and-jump 229, floor-below 121, no-flat-floor 192,
   hit-weapon 195, screen-shake 135, inherit-Kurt-velocity 170, sky-mode
   202, camera-track 203 and place-near-Kurt 220 are implemented and pinned.
   Opcode 170 reads the already-ported `Player` live at dispatch time;
   `ScriptRuntime` has no defaulted Player argument, so every construction
   states the choice. A frontend must still bind the live Player for
   170/203/220 to be complete. Opcode 202 writes shared
   `ScriptRuntime::skyMode`, but its renderer backdrop and snapshot consumers
   remain deferred. Opcode 203 writes the runtime camera pitch and two-tick
   lifetime; its follow-camera and snapshot consumers remain deferred. Opcode
   220 writes the receiver's X over the seeded RNG/object-list seams and uses
   the runtime's observed `KurtVelocity` field for its predicted-X branch. The
   current-plus-second raycast selection seam is pinned against
   overlapping DANT_1/DANT_8 geometry. All three oracle `Linked` writers
   (245/179/228) are ported, so partner pairs can feed the 213/214 conditions.
   132/136 still need Effects; frame-blend integration is a separate future
   slice. The exact remaining 29 are in `docs/OPCODE_COVERAGE.md`.
4. **P5.0 — frame-golden harness** (`SPEC-COMPLETION.md` §3 item 4) before
   renderer batches. The reference source is DECIDED: the C# renderer,
   headlessly under Linux/lavapipe (see `SPEC-COMPLETION.md`). The harness
   itself is not built; no renderer batch until its gate is green.

Deferred: 248 `push_kurt` is an honest no-op pending Kurt knock-back. The
C#'s Rides subsystem also writes the alarm countdown and remains deferred with
Rides. Opcode 135's follow-camera consumer, opcode 202's renderer backdrop
and snapshot consumers, opcode 203's follow-camera/snapshot consumers and
missing frontend `Player` binding, opcode 195's group-hit/direct combat
writers, opcode 81's hit-direction writers, and opcodes 170/220's missing
frontend `Player` binding remain partial with those integrations. Old-3DS
timing remains UNMEASURED.

## Current tree state (VERIFIED)

- Opcode 220 (`place_x_near_player`) now dispatches to
  `ScriptRuntime::placeNearKurt`, matching `ScriptVm.cs:1121-1123` and
  `ScriptRuntime.cs:1136-1186`. The first seeded draw selects the random,
  current-X or predicted-X branch; only random consumes a second draw. An X
  below the minimum wraps once, then up to 100 passes move it twelve units
  left of live same-arena, same-Y objects. Removed C++ deque slots are skipped
  to mirror the C# live `Objects` list.
- The runtime now has the C#'s observed `ScriptRuntime::KurtVelocity` field:
  copied feet delta × 30, written before object scripts as in
  `ScriptRuntime.cs:945-946`. It is distinct from the live `Kurt.Velocity`
  used by opcode 170, so the latter accessor is now `kurtLiveVelocity()`.
- `PlaceNearKurtChoosesWrapsAndSpaces` pins the observed-velocity tick
  update, all three X choices, range/Y fallbacks, wrap, dead/cross-arena/Y
  exclusions, ordered spacing pushes and the following instruction.
- Coverage is now 209 true dispatches / 238 with 29 remaining. The
  brace-matched recount gives C# 238 cases and C++ 211 numeric labels / 210
  unique labels; opcode 249's nested `case 0`/`case 1` arms account for the
  extra raw label and the duplicate. Classification: 17 partial, 192 complete.
- Verification on this tree:
  - `make -C tests/scripts clean && make -C tests/scripts` exited 0 after 44
    VM tests and the complete oracle suite (`All cases match the oracle.`).
  - `bash tools/check.sh` exited 0 with all contract gates (`CHECK: ALL GATES
    PASSED`); SMDH was skipped because no CIA was built.
  - `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds` exited 0, linked `mdk.elf`, staged 69 RomFS files (79M), and built
    `mdk.3dsx`.
  - `cmake --build build-linux --target mdk` exited 0 and built the Linux target.
  - `MDK_DATA_DIR=<repo>/MDK tests/scripts/build/tick1 probe 30000` exited 0
    with no unimplemented opcodes on levels 3-8.
- No golden changed; opcode 220 does not fire in the level 3-8 corpus. No
  emulator or hardware run was performed. Old-3DS timing remains unmeasured.
- Next isolated opcode family: port 219 (`if_gun_aim`) over the existing
  yaw/object-list seams and the new observed `KurtVelocity` field. Use a
  seeded synthetic test for the aim/fire branches. Before opcodes 170/203/220
  can be complete, frontend integration must construct `ScriptRuntime` and
  bind the live `Player`; do not re-port Kurt movement.

## Testability and standing open items

- Oracle/goldens compare fixed table + script fields, not velocity,
  `alarm_ticks`, `Items.Bomb`/`Decoy`, or all combat/effect state. Use
  `vm_tests` for otherwise unreachable opcode behavior; don't port without a
  test. The tick corpus matches byte-for-byte through t30000 on levels 3-8.
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
