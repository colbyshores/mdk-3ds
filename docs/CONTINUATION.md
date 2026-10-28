# Phase 2 continuation checkpoint

Last updated: 2026-10-27 (iteration 58: preserve dead partner links).
Current coverage: 196 true C++ dispatches / 238, 42 remaining. Continue one
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
   implemented and pinned. Newer slices include 85 (`set_rolling`), 74
   (`attach_to`), the 213/214 partner condition pair, and the 179/180/228
   partner-link family. All three oracle `Linked` writers (245/179/228) are
   now ported, so partner pairs can feed the 213/214 conditions. 132/136
   still need Effects; frame-blend integration remains a separate future
   slice. Remaining 42 are listed in `docs/OPCODE_COVERAGE.md`.
4. **P5.0 — frame-golden harness** (`SPEC-COMPLETION.md` §3 item 4) before
   renderer batches. Owner must select headless C# vs original-EXE reference;
   no renderer batch until the harness gate is green.

Deferred: alarm/move 15 fires zero times in current corpus; 248 `push_kurt` is
an honest no-op pending Kurt knock-back. Old-3DS timing remains UNMEASURED.

## Current tree state (VERIFIED)

- Iteration 58 addresses review-1's partner-widow findings.
  `ScriptRuntime::remove` now retains the oracle's inbound `Linked` pointer
  instead of nulling it, so removing one member of a pair leaves the survivor
  linked to its dead partner. `link_partners` consequently skips that widow exactly as
  the C# does, while variable source 3 still reads and writes the removed
  partner's persistent variable array. The `vm_tests` fixture now creates a
  real pair and calls `runtime.remove(partner)` instead of fabricating an
  unreachable `dead` flag; it pins the retained link, no re-pairing, and the
  removed partner's variable write. Coverage remains 196 true dispatches /
  238 with 42 remaining. `make -C tests/scripts clean && make -C
  tests/scripts` passed 28 VM tests and the complete oracle suite (`All cases
  match the oracle.`); `bash tools/check.sh` passed all contract gates (SMDH
  skipped, no CIA); `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
  make -C mdk_3ds` built `mdk.3dsx` after staging 69 RomFS files (79M), and
  `cmake --build build-linux --target mdk` built the Linux target. No golden
  changed; no emulator/hardware run. Old-3DS timing remains unmeasured.
- Iteration 57 ports the partner-link family: 179 (`spawn_partner`) spawns
  the named type at the receiver's position with yaw zero and links both
  objects; 180 (`snap_to_partner`) copies a live partner's position and then
  applies its optional offset; 228 (`link_partners`) pairs live same-arena
  objects of the named type in stable list order when their squared distance
  is below 10000, while its list form with a zero second value clears only
  the receiver's link. Three `vm_tests` cases pin both link directions,
  dead/no-link behavior, case-insensitive type matching, dead/cross-arena/
  already-linked exclusions, the strict 100-unit boundary, list order and
  the asymmetric clear. Coverage is now 196 true dispatches / 238 with 42
  remaining; all three oracle `Linked` writers are ported, removing the
  producer caveat from 213/214. `make -C tests/scripts clean && make -C
  tests/scripts` passed 28 VM tests and the complete oracle suite (`All cases
  match the oracle.`); `bash tools/check.sh` passed all contract gates (SMDH
  skipped, no CIA); `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
  make -C mdk_3ds` built `mdk.3dsx` after staging 69 RomFS files (79M), and
  `cmake --build build-linux --target mdk` built the Linux target. No golden
  changed; no emulator/hardware run. Old-3DS timing remains unmeasured.
- Iteration 56 addresses review-3's partner-condition fixture finding. The
  213/214 `vm_tests` case now gives the receiver a non-origin position and a
  non-zero yaw, makes the partner differ in z, separates the receiver from
  Kurt, and uses approximate-equality comparisons. It pins the full 3D
  distance, the receiver-relative origin, the floor of one, a live due-east
  angle of zero, and `wrap(receiverYaw - bearingToPartner)`: the negated turn
  required to face the partner. That single wording replaces the three contradictory descriptions in
  the test and documentation. `docs/OPCODE_COVERAGE.md` recorded that 213/214
  were complete through the existing `linked` field but were fed in integrated
  play only by the then-ported `find_object` writer (245); iteration 57 ports
  the other two writers (`spawn_partner` 179 and `link_partners` 228). Runtime
  behavior and oracle goldens are unchanged. `make -C tests/scripts clean && make -C tests/scripts` passed
  25 VM tests and the complete oracle suite (`All cases match the oracle.`);
  `bash tools/check.sh` passed all contract gates (SMDH skipped, no CIA);
  `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds`
  built `mdk.3dsx` after staging 69 RomFS files (79M). No golden changed; no
  emulator/hardware run. Old-3DS timing remains unmeasured.
- Iteration 55 ports opcodes 213 (`if_partner_distance`) and 214
  (`if_partner_angle`) over the existing `linked` field: zero without a live
  partner, distance floored at one, and angle
  `wrap(receiverYaw - bearingToPartner)`, the negated turn required to face the
  partner. It also addresses review-2's opcode-241 fixture finding: that test
  now builds its own `ScriptRuntime` instead of damaging the shared one, and
  the stale in-code/doc causal claims are corrected.
- Next: continue opcode conversion from the remaining 42 in
  `docs/OPCODE_COVERAGE.md`; select the next testable subsystem slice.

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
