# Phase 2 continuation checkpoint

Last updated: 2026-10-28 (opcode 219 gun-aim state + the FP-contraction
measurement).
Current coverage: 210 true C++ dispatches / 238, 28 remaining. Continue one
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
   202, camera-track 203, place-near-Kurt 220 and gun-aim 219 are implemented
   and pinned. Opcode 170 reads the already-ported `Player` live at dispatch
   time; `ScriptRuntime` has no defaulted Player argument, so every
   construction states the choice. A frontend must still bind the live Player
   for 170/203/219/220 to be complete. Opcode 202 writes shared
   `ScriptRuntime::skyMode`, but its renderer backdrop and snapshot consumers
   remain deferred. Opcode 203 writes the runtime camera pitch and two-tick
   lifetime; its follow-camera and snapshot consumers remain deferred. Opcode
   220 writes the receiver's X over the seeded RNG/object-list seams and uses
   the runtime's observed `KurtVelocity` field for its predicted-X branch.
   Opcode 219 aims the gun at Kurt's 1.67 s lead with the oracle's jitter,
   raised-target pick and fire rolls, and reads the same observed field. All
   three oracle `Linked` writers (245/179/228) are ported, so partner pairs can
   feed the 213/214 conditions. 132/136 still need Effects; frame-blend
   integration is a separate future slice. The exact remaining 28 are in
   `docs/OPCODE_COVERAGE.md`.
4. **P5.0 — frame-golden harness** (`SPEC-COMPLETION.md` §3 item 4) before
   renderer batches. The reference source is DECIDED: the C# renderer,
   headlessly under Linux/lavapipe (see `SPEC-COMPLETION.md`). The harness
   itself is not built; no renderer batch until its gate is green.

Deferred: 248 `push_kurt` is an honest no-op pending Kurt knock-back. The
C#'s Rides subsystem also writes the alarm countdown and remains deferred with
Rides. Opcode 135's follow-camera consumer, opcode 202's renderer backdrop and
snapshot consumers, opcode 203's follow-camera/snapshot consumers, and the
missing frontend `Player` binding that gates 170/203/219/220, opcode 195's
group-hit/direct combat writers and opcode 81's hit-direction writers remain
partial with those integrations. Old-3DS timing remains UNMEASURED.

## Current tree state (VERIFIED)

- Opcode 219 (`if_gun_aim`) dispatches to `ScriptRuntime::gunAim`, matching
  `ScriptVm.cs:1119-1120` and `ScriptRuntime.cs:1084-1134`: the bearing to Kurt
  led by 1.67 s of observed sideways velocity plus a quarter-degree jitter; a
  roll of 70 against `round(|vx|) + 10` (halves away from zero) fires at Kurt;
  otherwise up to four live same-arena objects within 2 in Y and 3 in Z of the
  target row and inside the 270°±3 cone are collected and one bearing is picked
  at random; failing that the gun faces 270° and fires on 29% of rolls.
- `GunAimAimsAndFires` pins 21 outcomes, and **every one of its expectations was
  reproduced against the C# oracle itself** in this batch (a scratch driver
  calling `ScriptRuntime.GunAim` over the same geometry and seeds; recipe in
  `docs/PROGRESS_LOG.md`). All 21 matched on fired flag, yaw to 4 dp and the
  following RNG draw, so the pin is oracle output, not C++ output.
- `mdk_3ds/Makefile` no longer passes `-ffp-contract=off` to
  `script_runtime.o`. Measured with the shipped flags: the TU compiles to a
  byte-identical object under the default, `-ffp-contract=off` and
  `-ffp-contract=fast` (164 `vmla`/`vmls`, 0 `vfma` each). VFPv2 has no fused
  multiply-add — GCC's `VFMA` needs `-mfpu=vfpv4` — so the flag could not change
  this target's code, only imply a `VMLA` rounding that has never been measured.
  Whether ARM11 `VMLA` rounds the product before the add is still UNMEASURED on
  hardware; that open question, not the flag, is what a future device capture
  has to settle (review-18 MINOR-1 closed on its premise).
- Coverage recount (brace-matched both dispatch methods): C# 238 cases, C++ 212
  numeric labels / 211 unique / 210 true dispatches, 28 remaining. The nested
  `case 0`/`case 1` arms inside opcode 249's switch are the extra raw label and
  the duplicate. Classification: 18 partial, 192 complete.
- Verification on this tree:
  - `make -C tests/scripts clean && make -C tests/scripts` exited 0 after 45
    VM tests (including `GunAimAimsAndFires`) and the complete oracle suite
    (`All cases match the oracle.`, 354 golden cases).
  - `bash tools/check.sh` exited 0 with all contract gates (`CHECK: ALL GATES
    PASSED`); SMDH was skipped because no CIA was built.
  - `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds` exited 0, recompiled `script_runtime.cpp`, linked `mdk.elf`,
    staged 69 RomFS files (79M), and built `mdk.3dsx`.
  - `cmake --build build-linux --target mdk` exited 0 and built the Linux target.
  - `MDK_DATA_DIR=<repo>/MDK tests/scripts/build/tick1 probe 30000` exited 0
    with no unimplemented opcodes on levels 3-8.
- No golden changed; opcode 219 does not fire in the level 3-8 corpus. No
  emulator or hardware run was performed. Old-3DS timing remains unmeasured.
- Next isolated opcode family: 174 (`if_ammo`, `ScriptVm.cs:999-1005`) over
  `Kurt`'s inventory seam — it needs the C# `Inventory.SuperChainGun` tick
  counter and the five `Inventory.Ammo` entries as fields, and their pickup
  writers stay deferred, so it lands partial like 165/166/195. The other 27
  remaining opcodes each need a deferred subsystem (Effects, Fans, Rides,
  SniperRounds, teleport, HUD messages, wind, waypoints, arena preload); do not
  blind-port them. The alternative increment with more value: construct
  `ScriptRuntime` in a shipped frontend and bind the live `Player`, which
  completes 170/203/219/220 at once. Do not re-port Kurt movement.

## Testability and standing open items

- Oracle/goldens compare fixed table + script fields, not velocity,
  `alarm_ticks`, `Items.Bomb`/`Decoy`, or all combat/effect state. Use
  `vm_tests` for otherwise unreachable opcode behavior; don't port without a
  test. The tick corpus matches byte-for-byte through t30000 on levels 3-8.
- Oracle driver: `/tmp/kilo/oracle/Driver/Program.cs`; run with
  `DOTNET_ROOT=/tmp/kilo/dotnet10`, `MDK_DATA_DIR=<repo>/MDK`, driver args
  `L T 1 script`. Source oracle `src/Mdk.Game/**` is read-only. To call a
  runtime method directly (the 219 route), copy `src/`, `assets/` and
  `Directory.Build.props` to a scratch tree plus `src/Mdk.Engine/obj/shaders`,
  and build with `NUGET_PACKAGES=/home/coleshores/.nuget/packages dotnet build
  -p:PrecompiledShaders=spirv`; the repo's own C# `obj/` is not writable here.
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
