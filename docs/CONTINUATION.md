# Phase 2 continuation checkpoint

Last updated: 2026-10-28 (iteration 63: opcode 229, turn and jump).
Current coverage: 201 true C++ dispatches / 238, 37 remaining. Continue one
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
   204, and turn-and-jump 229 are implemented and pinned. All three oracle
   `Linked` writers (245/179/228) are ported, so partner pairs can feed the
   213/214 conditions. 132/136 still need Effects; frame-blend integration is a
   separate future slice. The exact remaining 37 are in
   `docs/OPCODE_COVERAGE.md`.
4. **P5.0 — frame-golden harness** (`SPEC-COMPLETION.md` §3 item 4) before
   renderer batches. Owner must select headless C# vs original-EXE reference;
   no renderer batch until the harness gate is green.

Deferred: 248 `push_kurt` is an honest no-op pending Kurt knock-back. The
C#'s Rides subsystem also writes the alarm countdown and remains deferred with
Rides. Old-3DS timing remains UNMEASURED.

## Current tree state (VERIFIED)

- Iteration 63 ports opcode 229 (`turn_and_jump_to_dest`) and movement command
  229. The receiver turns toward its move destination by at most the requested
  degrees per second; once facing it, the oracle's ballistic velocity, apex,
  total flight time and friction-adjusted horizontal speed are set. Movement
  command 229 advances the flight timer and stops with zeroed velocity on floor
  contact after the apex. One synthetic `vm_tests` case pins the turn limit,
  false/true branches, launch constants, first motion tick, early-contact
  exclusion and floor landing. Mutation-checked: doubling the turn rate,
  shortening the arc, dropping the friction compensation, allowing a landing
  before the apex and removing the landing stop each fail the test. Coverage is 201 true dispatches / 238 with 37
  remaining.
- Verification on this tree:
  - `make -C tests/scripts clean && make -C tests/scripts` passed 35 VM tests
    and the complete oracle suite (`All cases match the oracle.`).
  - `bash tools/check.sh` passed all contract gates (`CHECK: ALL GATES
    PASSED`); SMDH was skipped because no CIA was built.
  - `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds` linked `mdk.elf`, staged 69 RomFS files (79M), and built
    `mdk.3dsx`.
  - `cmake --build build-linux --target mdk` built the Linux target.
- No golden changed; opcode 229 does not fire in the level 3–8 corpus. No
  emulator or hardware run was performed. Old-3DS timing remains unmeasured.
- The newest external review,
  `logs/ralph-pi/run-20261027-223315/review-6.md`, contains only
  `REVIEWER FAILED model=litellm/qwen rc=1`; it supplied no BLOCKER, MAJOR,
  MINOR or NIT finding to address. Review-4's remaining documentation point
  was fixed in iteration 61.
- Next: port opcode 121 (`if_floor_below`) over the existing arena raycast
  seam, with a `vm_tests` case pinning clear/blocked rays and both branches.
  Keep opcode 192 (`if_no_flat_floor_at`) as a separate slice unless its hit
  normal can be carried by the same tested raycast seam.

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
