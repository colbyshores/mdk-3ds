# Phase 2 continuation checkpoint

Last updated: 2026-10-27 (iteration 46: ported opcode 74 `attach_to` and added
synthetic VM coverage). Current coverage: 190 true C++ dispatches / 238, 48
remaining. Iteration-46 verification is recorded below after required gates. Next: continue one opcode family per
batch via `vm_tests`.

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
   implemented and pinned (relevant slices detailed in `PROGRESS_LOG.md`).
   The newest batches are 239 (reference no-op; decoder consumes six floats),
   77 (`debug_msg`, ignored by reference), 85 (`set_rolling`; rotation math
   and transition test), and 74 (`attach_to`; tested name selection and points).
   132/136 still need Effects; frame-blend integration remains a separate future
   slice. Remaining 48 are listed in
   `docs/OPCODE_COVERAGE.md`.
4. **P5.0 — frame-golden harness** (`SPEC-COMPLETION.md` §3 item 4) before
   renderer batches. Owner must select headless C# vs original-EXE reference;
   no renderer batch until the harness gate is green.

Deferred: alarm/move 15 fires zero times in current corpus; 248 `push_kurt` is
an honest no-op pending Kurt knock-back. Old-3DS timing remains UNMEASURED.

## Current tree state (VERIFIED)

- Iteration 46 ports opcode 74 `attach_to`: selects the last live object in the
  same arena whose case-insensitive `TYPE_ID` matches, then assigns leader,
  attach points, and move command. The synthetic VM test pins successful match,
  case folding, last-match behavior, and miss-state preservation. No golden
  changed. Verification: `make -C tests/scripts clean && make -C tests/scripts`
  passed all 23 VM tests and all oracle cases (`All cases match the oracle.`);
  `bash tools/check.sh` reported all gates passed (`gates_smdh` skipped, no CIA);
  `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds`
  linked `mdk.elf`, staged 69 RomFS files (79M), and built `mdk.3dsx` without
  warnings. `git diff --check` passed. No emulator/hardware run; Old-3DS timing
  remains unmeasured.
- Iteration 45 corrected the `object.h` behavior inventory per review-4:
  sound positioning is owned by `SoundMixer`, distance is inlined at existing
  call sites, `PoseParts` remains unported, and the `PartCenter` distinction is
  explicit. No runtime behavior or golden changed; findings addressed below.
- Iteration 44 corrected the file-level `object.h` overview and reconciled its
  deferred behavior list against the tree: animation advance, pose bounds,
  part center, and runtime yaw-to are implemented; Footing, frame-blend/transform
  reads and pose/part baking remain deferred. Sound-position and distance-to
  behavior already exists elsewhere in the tree. Documentation-only; no behavior
  or test fixtures changed. This addresses external review-3's documentation drift.
- Iteration 43 removed implemented `Rotation` from the deferred list per review-2.
  External review-3 found that its new progress sentence and several other list
  entries were still inaccurate; this iteration audits and corrects those.
  `make -C tests/scripts clean && make -C tests/scripts` passed all 22 VM
  tests and all table/script/other oracle cases (`All cases match the oracle.`).
  `bash tools/check.sh` reported all gates passed; `gates_smdh` skipped because
  no `.cia` exists. `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
  make -C mdk_3ds` linked `mdk.elf`, staged 69 RomFS files (79M), and built
  `mdk.3dsx`. No emulator or hardware run was relevant/performed. The change
  was committed as `67c60a7` (one iteration-43 commit).
- Iteration 45 verification: `make -C tests/scripts clean && make -C
  tests/scripts` passed 22 VM tests and all oracle cases (`All cases match the
  oracle.`); `bash tools/check.sh` reported all gates passed (`gates_smdh`
  skipped because no `.cia` exists); `DEVKITPRO=/opt/devkitpro
  DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds` linked `mdk.elf`, staged
  69 RomFS files (79M), and built `mdk.3dsx`. `git diff --check` passed.
  Documentation-only correction; no emulator/hardware run. Old-3DS timing
  remains unmeasured. Batch committed as the iteration-45 commit.
- Iteration 44 verification: `make -C tests/scripts clean && make -C
  tests/scripts` passed all 22 VM tests and all oracle cases (`All cases match
  the oracle.`); `bash tools/check.sh` reported `CHECK: ALL GATES PASSED`
  (`gates_smdh` skipped because no `.cia` exists); and
  `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds`
  linked `mdk.elf`, staged 69 RomFS files (79M), and built `mdk.3dsx`.
  `git diff --check` passed. This documentation-only change required no emulator
  or hardware run; Old-3DS timing remains unmeasured. Code/docs committed as
  `aca61d5`; its verification record was committed separately as `96d8c31`.
- Iteration 42 corrected opcode 85's rotation rounding per external review and
  pins the full 4x4 matrix against .NET 10 oracle bits. Coverage: C# 238 unique
  cases; C++ 191 numeric labels, 190 unique (nested `case 0` in 249 is not an
  opcode), 189 true dispatches, 49 missing.
- `make -C tests/scripts clean && make -C tests/scripts` → all 22 VM tests
  PASS, then `All cases match the oracle.` (table/script corpus byte-identical).
- `bash tools/check.sh` → `CHECK: ALL GATES PASSED`; `gates_smdh` SKIPPED
  because no built `.cia` exists (not reported as passed).
- `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds`
  → linked `mdk.elf`, staged 69 RomFS files (79M), built `mdk.3dsx`.
- No actual Old-3DS hardware capture was performed; hardware readiness remains
  unmeasured.
- `git diff --check` → clean.
- External review `logs/ralph-pi/run-20261027-014607/review-1.md` identified
  degree conversion and `Matrix4x4` rounding divergences in opcode 85; the
  iteration-42 fix is pinned by exact bits. An ARM11 object disassembly shows
  `fmaf` calls (not an inline fused opcode); no actual 3DS hardware run was
  performed, so runtime/performance evidence remains open.

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
