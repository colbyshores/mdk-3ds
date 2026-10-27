# Phase 2 continuation checkpoint

Last updated: 2026-10-26 (iteration 39: opcode 239 `set_27c` explicit
no-op ported and pinned; C# operand layout is six F32s. `vm_tests` proves
that the following instruction executes and 239 is not reported unimplemented.
Review-37 failed for quota reasons; no newer review exists. Current coverage:
187 true C++ dispatches / 238, 51 remaining. All three required gates passed;
see VERIFIED below. Next: continue one opcode family per batch via `vm_tests`.

This is persistent handoff. Keep it under ~150 lines; batch narrative goes in
`docs/PROGRESS_LOG.md`.

## PRIORITY ORDER (owner directive 2026-10-25)

1. **DONE — mixer call-sites** (`ObjectMotion.cs:133`, `ObjectBehaviors.cs:90-115`).
2. **DONE — `Debris.Shatter` + opcodes 137/138/139**, dump line and oracle
   goldens. BreakUp/Trail/Updraft await Effects.
3. Continue opcode conversion subsystem by subsystem; tests/goldens in the same
   batch. Opcode 208 (reachability probe), harness + 13, parts 32/129,
   rope-swing 226/242, spawn 156/206, push-hit-dir 81, stats 217, vision 243,
   value slots 199/210, Kurt/screen 215/240/244 and unidentified no-op 239 are
   implemented and pinned (relevant slices detailed in `PROGRESS_LOG.md`).
   The newest batch is 239: the reference explicitly does nothing, but the
   decoder still consumes six floats. 132/136 still need Effects; 85 needs
   FrameBlend's `rotation()`; do not blind-port. Remaining 51 are listed in
   `docs/OPCODE_COVERAGE.md`.
4. **P5.0 — frame-golden harness** (`SPEC-COMPLETION.md` §3 item 4) before
   renderer batches. Owner must select headless C# vs original-EXE reference;
   no renderer batch until the harness gate is green.

Deferred: alarm/move 15 fires zero times in current corpus; 248 `push_kurt` is
an honest no-op pending Kurt knock-back. Old-3DS timing remains UNMEASURED.

## Current tree state (VERIFIED)

- Iteration 39, code commit pending until this iteration's doc update and gates
  are committed together. No golden changes: opcode 239 does not fire in
  levels 3-8. Coverage recount: C# 238 unique cases; C++ 189 numeric labels,
  188 unique (nested `case 0` in 249 is not an opcode), 187 true dispatches,
  51 missing.
- `make -C tests/scripts` → `vm_tests` 20/20 PASS, then `All cases match the
  oracle.` (table/script corpus byte-identical).
- `bash tools/check.sh` → `CHECK: ALL GATES PASSED`; `gates_smdh` SKIPPED
  because no built `.cia` exists (not reported as passed).
- `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds`
  → linked `mdk.elf`, staged 69 RomFS files (79M), built `mdk.3dsx`. A bare
  `make -C mdk_3ds` failed due to unset DEVKITARM; rerun with required env
  succeeded.
- Latest external review `review-37.md` is an environment failure (429
  subscription quota), no findings. `review-36.md` concerned older stats code;
  no new finding for this batch.

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
header changes. Keep the monolithic `ScriptVm::execute()` switch. Never weaken
a gate or test.
