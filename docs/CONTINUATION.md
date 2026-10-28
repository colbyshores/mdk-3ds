# Phase 2 continuation checkpoint

Last updated: 2026-10-28 (review-13 correction to opcode 170's Kurt-velocity source).
Current coverage: 206 true C++ dispatches / 238, 32 remaining. Continue one
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
   195, screen-shake 135 and inherit-Kurt-velocity 170 are implemented and
   pinned. Opcode 170 now reads the already-ported `Player` live at dispatch
  time; frontend integration must still construct `ScriptRuntime` and bind
  that Player before 170 is complete. The current-plus-second raycast
  selection seam is pinned against
   overlapping DANT_1/DANT_8 geometry. All three oracle `Linked` writers
   (245/179/228) are ported, so partner pairs can feed the 213/214 conditions.
   132/136 still need Effects; frame-blend integration is a separate future
   slice. The exact remaining 32 are in `docs/OPCODE_COVERAGE.md`.
4. **P5.0 — frame-golden harness** (`SPEC-COMPLETION.md` §3 item 4) before
   renderer batches. Owner must select headless C# vs original-EXE reference;
   no renderer batch until the harness gate is green.

Deferred: 248 `push_kurt` is an honest no-op pending Kurt knock-back. The
C#'s Rides subsystem also writes the alarm countdown and remains deferred with
Rides. Opcode 135's follow-camera consumer, opcode 195's group-hit/direct
combat writers, opcode 81's hit-direction writers, and opcode 170's missing
frontend `Player` binding remain partial with those integrations. Old-3DS
timing remains UNMEASURED.

## Current tree state (VERIFIED)

- The latest batch corrects opcode 170 (`inherit_kurt_vel`) after review-13.
  The receiver's velocity gains Kurt's current velocity multiplied by the F32
  operand. `ScriptRuntime` now borrows the already-ported `mdk::Player` and
  reads `Player::velocity()` live at dispatch time; the old copied
  `setKurtVelocity` snapshot is gone. `Player::velocity()` includes the C#
  `Kurt.cs:133` horizontal knock-down-push term and its drain, so a future
  combat writer cannot silently omit it. The real remaining integration gap is
  that no shipped frontend constructs `ScriptRuntime` and binds its Player
  yet; the dispatch therefore remains partial. One synthetic `vm_tests` case pins the
  dispatch, literal scaling, all three velocity components, preservation of
  the receiver's existing velocity, a negative scale, fall-through, a live
  Player change after runtime construction, the push term and its drain.
- Review-13's MAJOR finding and MINOR unit/documentation issue are fixed. The
  old record incorrectly said Kurt movement was unported; `mdk::Player` has
  carried that movement since the earlier player batches. The seam is now
  explicitly in u/s and distinct from the C# `ScriptRuntime.KurtVelocity`
  observed-position-delta field.
- Review-12's sole MINOR finding is fixed. The opcode-135 test now makes a
  finite request after a NaN request and pins that `MathF.Max(NaN, finite)`
  stays NaN. Mutation-checking the runtime `mathMax` first arm to return the
  finite new value makes `vm_tests` exit 1 with exactly that assertion failing.
- Review-12 found no BLOCKER or MAJOR finding. Its cutscene note remains a
  constraint on the future cutscene slice: the alarm decrement and screen-shake
  clear must move after the oracle's cutscene early return.
- Coverage is now 206 true dispatches / 238 with 32 remaining. The
  brace-matched recount gives C# 238 cases and C++ 208 numeric labels / 207
  unique labels; opcode 249's nested `case 0`/`case 1` arms account for the
  extra raw label and the duplicate.
- Verification on this tree:
  - `make -C tests/scripts clean && make -C tests/scripts` exited 0 after 41
    VM tests and the complete oracle suite (`All cases match the oracle.`).
  - `bash tools/check.sh` exited 0 with all contract gates (`CHECK: ALL GATES
    PASSED`); SMDH was skipped because no CIA was built.
  - `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds` exited 0, linked `mdk.elf`, staged 69 RomFS files (79M), and built
    `mdk.3dsx`.
  - `cmake --build build-linux --target mdk` exited 0 and built the Linux target.
  - `MDK_DATA_DIR=<repo>/MDK tests/scripts/build/tick1 probe 30000` exited 0
    and reported no unimplemented opcodes on levels 3–8.
- No golden changed; opcode 170 does not fire in the level 3–8 corpus. No
  emulator or hardware run was performed. Old-3DS timing remains unmeasured.
- Next isolated opcode family: port 202 (`set_574304`) with a runtime
  sky-mode seam and a synthetic test. Its renderer consumer remains deferred,
  so classify the dispatch partial until the sky renderer consumes it. Before
  opcode 170 can be complete, frontend integration must construct
  `ScriptRuntime` and bind the live `Player`; do not re-port Kurt movement.

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
